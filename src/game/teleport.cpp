#include "game/teleport.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"
#include "net/net.h"

namespace mecmp::game::teleport {
namespace {
constexpr DWORD kFadeMs = 350;      // fade to black, and back
constexpr DWORD kHoldMs = 1200;     // black while the target area streams in
constexpr float kFallMargin = 2.f;  // below the target by this much during the hold: placed again
constexpr int kMaxRetries = 6;
constexpr uint32_t kMaxAgeMs = 2000; // the target player must have been heard from this recently

enum Phase : int { Idle, FadeOut, Hold, FadeIn };
std::atomic<int> g_phase{Idle};
std::atomic<DWORD> g_phaseAt{0};
std::atomic<uint32_t> g_peer{0};
std::atomic<int> g_available{-1}; // -1 unchecked, 0 no, 1 yes
LinearTransform g_target{};       // game thread only
int g_retries = 0;

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
    static const uint8_t kExpect[] = {0x48, 0x8B, 0xF9, 0x48, 0x8D, 0x99, 0x10, 0x04, 0x00, 0x00};
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
} // namespace

void requestGoTo(uint32_t peer) {
    if (g_phase != Idle || !available()) return;
    g_peer = peer;
    setPhase(FadeOut);
}

bool busy() { return g_phase != Idle; }

bool available() { return g_available.load() != 0; }

float fadeAlpha() {
    const int p = g_phase.load();
    if (p == Idle) return 0.f;
    const float t = static_cast<float>(GetTickCount() - g_phaseAt.load());
    if (p == FadeOut) return t >= kFadeMs ? 1.f : t / kFadeMs;
    if (p == Hold) return 1.f;
    return t >= kFadeMs ? 0.f : 1.f - t / kFadeMs; // FadeIn
}

void levelChanged() {
    if (g_phase != Idle) MLOG("teleport: level changed; cancelled");
    g_phase = Idle;
}

void tick() {
    const int p = g_phase.load();
    if (p == Idle) return;
    const DWORD since = GetTickCount() - g_phaseAt.load();
    void* player = level::playerCharacter();
    if (p == FadeOut) {
        if (since < kFadeMs) return;
        net::PlayerState st;
        uint32_t age = 0;
        if (!player || !checkFunction() || !net::remoteState(g_peer, st, age) || age > kMaxAgeMs || !finite(st.pos, 3) ||
            !std::isfinite(st.yaw)) {
            MLOG("teleport: player %08x is not available right now; cancelled", g_peer.load());
            setPhase(FadeIn);
            return;
        }
        // On the player's own spot, facing their way: a ghost has no collision, and a spot beside them
        // could be off a ledge or inside a wall.
        g_target = transformFromYaw(st.pos, st.yaw);
        g_retries = 0;
        teleportTo(player, g_target);
        MLOG("teleport: to player %08x at %.1f %.1f %.1f", g_peer.load(), st.pos[0], st.pos[1], st.pos[2]);
        setPhase(Hold);
        return;
    }
    if (p == Hold) {
        LinearTransform now{};
        // Falling through ground that has not streamed in yet: put back while the screen is black.
        if (player && readEntityTransform(player, now) && now.trans[1] < g_target.trans[1] - kFallMargin &&
            g_retries < kMaxRetries) {
            ++g_retries;
            teleportTo(player, g_target);
            VLOG("teleport: placed again (%.1f m below the target, attempt %d)", g_target.trans[1] - now.trans[1], g_retries);
        }
        if (since >= kHoldMs) setPhase(FadeIn);
        return;
    }
    if (p == FadeIn && since >= kFadeMs) g_phase = Idle;
}

} // namespace mecmp::game::teleport
