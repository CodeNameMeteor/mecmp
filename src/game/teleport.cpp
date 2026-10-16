#include "game/teleport.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"
#include "game/raycast.h"
#include "net/net.h"

namespace mecmp::game::teleport {
namespace {
constexpr DWORD kFadeMs = 350;      // fade to black, and back
// Black while the target area streams in. The hold lasts at least kHoldMs and ends once the ground
// under the target has been found and the character stood on it for kSettleMs; with no ground by
// kMaxHoldMs we go back to where we started (falling through a city that is not there kills).
constexpr DWORD kHoldMs = 1200;
constexpr DWORD kSettleMs = 400;
constexpr DWORD kMaxHoldMs = 8000;
// The ground probe: a ray straight down from a little above the target. Hits above kProbeAccept
// over the target (the character's own capsule, a ceiling) are skipped by casting again below them.
constexpr float kProbeUp = 1.0f, kProbeDown = 40.f, kProbeAccept = 0.6f;
constexpr int kProbeCasts = 3;
constexpr float kFarGround = 2.f;   // ground further below the target than this waits for kHoldMs
constexpr float kLift = 0.05f;      // set down this much above the ground (the anchor can sit in a thin floor)
constexpr float kFallMargin = 0.5f; // below the target by this much during the hold: placed again
constexpr int kMaxRetries = 10;
constexpr float kNearTarget = 30.f; // further than this (horizontally): respawned elsewhere, leave it
// The screen must never stay black: a phase running this much longer than planned (the server tick
// stopped, e.g. while the game handles a death) ends the whole thing, drawn or not.
constexpr DWORD kStuckMs = 4000;
constexpr uint32_t kMaxAgeMs = 2000; // the target player must have been heard from this recently

enum Phase : int { Idle, FadeOut, Hold, FadeIn };
std::atomic<int> g_phase{Idle};
std::atomic<DWORD> g_phaseAt{0};
enum Source : int { Peer, Fixed, Back };
std::atomic<int> g_source{Peer};
std::atomic<uint32_t> g_peer{0};
// A summoned place (source Fixed), written before the phase leaves Idle so the game thread sees it.
float g_fixedPos[3] = {0, 0, 0};
float g_fixedYaw = 0;
// Where a summon took us from (written by the game thread in tick; g_backAt = when, 0 = nothing to go back to).
LinearTransform g_back{};
void* g_backCharacter = nullptr;
std::atomic<DWORD> g_backAt{0};
constexpr DWORD kBackMs = 60000;  // how long going back stays possible
std::atomic<int> g_available{-1}; // -1 unchecked, 0 no, 1 yes
LinearTransform g_target{};       // game thread only
void* g_character = nullptr;      // the character that was moved (game thread)
int g_retries = 0;
LinearTransform g_from{};         // where this teleport started (game thread)
bool g_haveFrom = false;
DWORD g_groundAt = 0;             // when the ground under the target was found (0 = not yet)
DWORD g_steadyAt = 0;             // since when the character stands on it (0 = not)

void setPhase(Phase p) {
    g_phaseAt = GetTickCount();
    g_phase = p;
}

// ServerCharacterEntity_teleport (0x143378830): (character, const LinearTransform* target). Stores the
// target (+0x410), moves the physics controller (+0x60, vtbl+0x140) and zeroes its velocity
// (vtbl+0x168), sets the transform, sends CharacterToComponentsOnTeleportedMessage to the
// character's components and marks it dirty for replication. The target is read with MOVAPS, so it
// must be 16-byte aligned (LinearTransform is).
// Checked against the code: "mov rdi,rcx; lea rbx,[rcx+410h]" 24 bytes in.
bool checkFunction() {
    int a = g_available.load();
    if (a >= 0) return a == 1;
    static const uint8_t kExpect[] = {0x48, 0x89, 0xCF, 0x48, 0x8D, 0x99, 0x10, 0x04, 0x00, 0x00};
    uint8_t got[sizeof(kExpect)] = {};
    const bool ok = mem::read(rva(Fn_ServerCharacterEntity_teleport) + 24, got, sizeof(got)) && memcmp(got, kExpect, sizeof(got)) == 0;
    if (!ok) {
        char hex[64] = {};
        for (size_t i = 0; i < sizeof(got); ++i) snprintf(hex + i * 3, 4, "%02X ", got[i]);
        MLOG("teleport: the character teleport function looks different in this build (%s); 'Go to' is off", hex);
    }
    g_available = ok ? 1 : 0;
    return ok;
}

void teleportTo(void* player, const LinearTransform& t) {
    alignas(16) LinearTransform x = t;
    reinterpret_cast<void (*)(void*, const LinearTransform*)>(rva(Fn_ServerCharacterEntity_teleport))(player, &x);
}

bool finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 100000.f) return false;
    return true;
}

// The height of the ground under `t` (false: nothing there yet, or no ray caster in this build).
bool groundBelow(const LinearTransform& t, float& ground) {
    if (!raycast::available()) return false;
    float from[3] = {t.trans[0], t.trans[1] + kProbeUp, t.trans[2]};
    const float to[3] = {t.trans[0], t.trans[1] - kProbeDown, t.trans[2]};
    for (int i = 0; i < kProbeCasts; ++i) {
        raycast::Hit hit;
        if (!raycast::cast(from, to, hit)) return false;
        if (hit.pos[1] <= t.trans[1] + kProbeAccept) {
            ground = hit.pos[1];
            return true;
        }
        from[1] = hit.pos[1] - 0.05f; // above the target: our own capsule or a ceiling, look below it
        if (from[1] <= to[1]) return false;
    }
    return false;
}
} // namespace

// No teleport in or out of a run (a dash, a time trial): it would cheat the time. Every way in (the menu,
// the players panel, a summon, "Go back?", an answer to "may I come?") ends here, so the rule holds even
// where the caller forgot to check.
static bool refusedInRun(const char* what) {
    if (!net::inRun()) return false;
    MLOG("teleport: %s refused: you are in a run", what);
    return true;
}

void requestGoTo(uint32_t peer) {
    if (g_phase != Idle || !available() || refusedInRun("go to")) return;
    g_source = Peer;
    g_peer = peer;
    setPhase(FadeOut);
}

bool requestToPos(const float pos[3], float yaw) {
    if (g_phase != Idle || !available() || !finite(pos, 3) || !std::isfinite(yaw) || refusedInRun("summon")) return false;
    memcpy(g_fixedPos, pos, sizeof(g_fixedPos));
    g_fixedYaw = yaw;
    g_source = Fixed;
    setPhase(FadeOut);
    return true;
}

uint32_t returnLeftMs() {
    const DWORD at = g_backAt.load();
    if (!at) return 0;
    const DWORD since = GetTickCount() - at;
    return since >= kBackMs ? 0 : kBackMs - since;
}

bool requestReturn() {
    if (g_phase != Idle || !available() || returnLeftMs() == 0 || refusedInRun("go back")) return false;
    g_source = Back;
    setPhase(FadeOut);
    return true;
}

void forgetReturn() {
    if (g_phase == Idle) g_backAt = 0; // (a return already under way finishes as asked)
}

bool busy() { return g_phase != Idle; }

bool available() { return checkFunction(); } // checked once (on the first call), then remembered

float fadeAlpha() {
    const int p = g_phase.load();
    if (p == Idle) return 0.f;
    const DWORD since = GetTickCount() - g_phaseAt.load();
    const DWORD planned = p == Hold ? kMaxHoldMs : kFadeMs;
    if (since > planned + kStuckMs) { // nothing moved it on: give the screen back
        int expected = p;
        if (g_phase.compare_exchange_strong(expected, Idle))
            MLOG("teleport: stopped waiting (the game did not run for %lu ms); screen restored", static_cast<unsigned long>(since));
        return 0.f;
    }
    const float t = static_cast<float>(since);
    if (p == FadeOut) return t >= kFadeMs ? 1.f : t / kFadeMs;
    if (p == Hold) return 1.f;
    return t >= kFadeMs ? 0.f : 1.f - t / kFadeMs; // FadeIn
}

void levelChanged() {
    if (g_phase != Idle) MLOG("teleport: level changed; cancelled");
    g_phase = Idle;
    g_backAt = 0; // the place it would go back to is another level now
}

void tick() {
    const int p = g_phase.load();
    if (p == Idle) {
        // Going back is only for the character that was moved; it died, respawned or the level changed.
        if (g_backAt.load() && returnLeftMs() == 0) g_backAt = 0;
        else if (g_backAt.load() && level::playerCharacter() != g_backCharacter) g_backAt = 0;
        return;
    }
    const DWORD since = GetTickCount() - g_phaseAt.load();
    void* player = level::playerCharacter();
    if (p == FadeOut) {
        if (since < kFadeMs) return;
        const int source = g_source.load();
        if (!player || !checkFunction()) {
            MLOG("teleport: the player character is not available right now; cancelled");
            setPhase(FadeIn);
            return;
        }
        if (source == Peer) {
            net::PlayerState st;
            uint32_t age = 0;
            if (!net::remoteState(g_peer, st, age) || age > kMaxAgeMs || !finite(st.pos, 3) || !std::isfinite(st.yaw)) {
                MLOG("teleport: player %08x is not available right now; cancelled", g_peer.load());
                setPhase(FadeIn);
                return;
            }
            // On the player's own spot, facing their way: a ghost has no collision, and a spot beside them
            // could be off a ledge or inside a wall.
            g_target = transformFromYaw(st.pos, st.yaw);
            MLOG("teleport: to player %08x at %.1f %.1f %.1f", g_peer.load(), st.pos[0], st.pos[1], st.pos[2]);
        } else if (source == Fixed) {
            if (!finite(g_fixedPos, 3) || !std::isfinite(g_fixedYaw)) {
                MLOG("teleport: the summoned place is not a place; cancelled");
                setPhase(FadeIn);
                return;
            }
            // Where we stood, so that a summon can be undone for a minute (returnLeftMs / requestReturn).
            LinearTransform before{};
            // A transform that is not a place (read during a level load) must not become the way back.
            if (readEntityTransform(player, before) && finite(before.trans, 3)) {
                g_back = before;
                g_backCharacter = player;
                g_backAt = GetTickCount() | 1; // never 0
            }
            g_target = transformFromYaw(g_fixedPos, g_fixedYaw);
            MLOG("teleport: to a summoned place %.1f %.1f %.1f", g_fixedPos[0], g_fixedPos[1], g_fixedPos[2]);
        } else { // Back
            if (!g_backAt.load() || player != g_backCharacter) {
                MLOG("teleport: nothing to go back to (the character changed); cancelled");
                g_backAt = 0;
                setPhase(FadeIn);
                return;
            }
            g_target = g_back;
            g_backAt = 0;
            MLOG("teleport: back to where the summon took us from");
        }
        g_retries = 0;
        g_character = player;
        g_haveFrom = source != Back && readEntityTransform(player, g_from) && finite(g_from.trans, 3);
        g_groundAt = g_steadyAt = 0;
        teleportTo(player, g_target);
        setPhase(Hold);
        return;
    }
    if (p == Hold) {
        // Killed or respawned meanwhile (e.g. the other player stood somewhere deadly): the game is in
        // charge now. Never fight it; just give the screen back.
        if (player != g_character) {
            MLOG("teleport: the player character changed (death or respawn); not placing again");
            setPhase(FadeIn);
            return;
        }
        LinearTransform now{};
        const bool have = player && readEntityTransform(player, now);
        const float dx = now.trans[0] - g_target.trans[0], dz = now.trans[2] - g_target.trans[2];
        if (have && dx * dx + dz * dz >= kNearTarget * kNearTarget) { // respawned elsewhere: leave it
            setPhase(FadeIn);
            return;
        }
        const DWORD tickNow = GetTickCount();
        if (!g_groundAt) {
            // Until the ground under the target has streamed in, keep the character there (each
            // teleport also zeroes its velocity), like a fly mode waiting for the city.
            float ground = 0;
            if (!raycast::available()) { // no probe in this build: the target's own height, as before
                g_groundAt = tickNow | 1;
            } else if (groundBelow(g_target, ground) && (ground >= g_target.trans[1] - kFarGround || since >= kHoldMs)) {
                // Ground far below (the caller was in the air, or a floor near the target has not
                // streamed in yet): only taken after the minimum hold, when that floor had time to appear.
                VLOG("teleport: ground found %.2f m %s the target after %lu ms", std::fabs(ground - g_target.trans[1]),
                     ground <= g_target.trans[1] ? "below" : "above", static_cast<unsigned long>(since));
                g_target.trans[1] = ground + kLift;
                g_groundAt = tickNow | 1;
                teleportTo(player, g_target);
            } else if (since < kMaxHoldMs) {
                teleportTo(player, g_target);
            } else {
                if (g_haveFrom) {
                    MLOG("teleport: no ground at the destination after %lu ms; went back", static_cast<unsigned long>(since));
                    teleportTo(player, g_from);
                } else {
                    MLOG("teleport: no ground at the destination after %lu ms", static_cast<unsigned long>(since));
                }
                setPhase(FadeIn);
            }
            return;
        }
        // On the ground now: put back if falling (a floor too thin for the anchor, or still streaming).
        if (have && now.trans[1] < g_target.trans[1] - kFallMargin) {
            g_steadyAt = 0;
            if (g_retries < kMaxRetries) {
                ++g_retries;
                teleportTo(player, g_target);
                VLOG("teleport: placed again (%.1f m below the target, attempt %d)", g_target.trans[1] - now.trans[1], g_retries);
            }
        } else if (have && !g_steadyAt) {
            g_steadyAt = tickNow | 1;
        }
        const bool settled = g_steadyAt && tickNow - g_steadyAt >= kSettleMs;
        if ((since >= kHoldMs && settled) || since >= kMaxHoldMs) {
            if (!settled) MLOG("teleport: the character did not settle at the destination (%d placements)", g_retries);
            setPhase(FadeIn);
        }
        return;
    }
    if (p == FadeIn && since >= kFadeMs) g_phase = Idle;
}

} // namespace mecmp::game::teleport
