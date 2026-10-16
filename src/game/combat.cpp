#include "game/combat.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/level.h"
#include "game/player_audio.h"
#include "game/strike_math.h"
#include "game/teleport.h"
#include "net/net.h"
#include "net/player_audio.h"
#include "net/protocol.h"

namespace mecmp::game::combat {
namespace {
namespace proto = net::protocol;

struct Strike {
    uint8_t kind;
    uint8_t source;
    uint32_t event;
    DWORD at;
};
std::mutex g_m;
std::deque<Strike> g_strikes; // captured on the client thread, decided on the server tick
std::string g_notice;
int g_eventLogs = 0;

strike::HitLimiter g_limiter;
DWORD g_lastStrikeAt = 0;
bool g_haveStrike = false;

// Health watch (diagnostic): the player's hp, logged when it changes.
float g_lastHp = -1.f;
void* g_lastHealth = nullptr;

int indexOfSound(const char* name) {
    for (size_t i = 0; i < net::kPlayerSoundCount; ++i)
        if (!strcmp(net::kPlayerSounds[i], name)) return static_cast<int>(i);
    return -1;
}
int indexOfVocal(const char* name) {
    for (size_t i = 0; i < net::kPlayerVocalCount; ++i)
        if (!strcmp(net::kPlayerVocalWaves[i], name)) return static_cast<int>(net::kPlayerSoundCount + i);
    return -1;
}
const char* sourceName(uint8_t source) {
    if (source < net::kPlayerSoundCount) return net::kPlayerSounds[source];
    if (source < net::kPlayerAudioSourceCount) return net::kPlayerVocalWaves[source - net::kPlayerSoundCount];
    return "?";
}

// The player's health component (the human one), or null.
void* healthOf(void* character) {
    void* hc = getComponent(character, TI_ServerCharacterHealthComponent);
    if (!hc) return nullptr;
    uintptr_t vt = 0;
    if (!mem::read(reinterpret_cast<uintptr_t>(hc), vt) || vt != rva(Vtbl_PamServerHumanHealthComponent)) return nullptr;
    return hc;
}

bool readHp(void* hc, float& hp, float* maxHp = nullptr) {
    if (!mem::read(reinterpret_cast<uintptr_t>(hc) + Health_Current, hp)) return false;
    if (maxHp && !mem::read(reinterpret_cast<uintptr_t>(hc) + Health_Max, *maxHp)) return false;
    return std::isfinite(hp);
}


#if MECMP_DEBUG_KEYS
// REMOVE LATER: this whole probe block (and installProbe() in watchHealth) is reverse-engineering scaffolding for the
// open "no death animation" question (see CHANGELOG, Known limit). Developer builds only; delete it once that is solved.
// --- Death probe (diagnostic, developer builds only): how does the game kill the player? ---------------------
// Tested 2026-10-16: our setHealth(0) leaves the player at hp 0, never dead and never respawned. The human
// health component's vtable slots setHealth (+0xf0), applyDamage (+0x100) and die (+0x130) are swapped for
// loggers that print who called them (return addresses as image offsets), the health and the DamageInfo, then
// call the original. A natural death (a fall, an enemy) shows the path the game uses.
thread_local bool t_ours = false; // a call made by this mod (death probe)
uintptr_t g_origSetHealthAddr = 0, g_origDieAddr = 0; // the game's own functions, once the probe replaced the slots (which then lie outside the image)
constexpr size_t kSlotApplyDamage = 0x100, kSlotDie = 0x130;
using SetHealthFn = void (*)(void*, float);
using Fn3 = uintptr_t (*)(void*, uintptr_t, uintptr_t);
using Fn4 = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t);
SetHealthFn g_origSetHealth = nullptr;
Fn4 g_origApplyDamage = nullptr;
Fn3 g_origDie = nullptr;
bool g_probeInstalled = false;
std::atomic<int> g_probeLogs{0};

void probeLog(const char* what, void* hc, const char* extra) {
    if (g_probeLogs.fetch_add(1) >= 120) return;
    void* frames[10] = {};
    const USHORT n = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
    char trace[256] = {};
    size_t len = 0;
    const uintptr_t base = mem::moduleBase();
    for (USHORT i = 0; i < n && len + 20 < sizeof(trace); ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        len += static_cast<size_t>(snprintf(trace + len, sizeof(trace) - len, a >= base && a < base + 0x727b000 ? " +%llx" : " ?%llx",
                                            static_cast<unsigned long long>(a >= base && a < base + 0x727b000 ? a - base : a)));
    }
    float hp = -1.f;
    readHp(hc, hp);
    const uintptr_t h = reinterpret_cast<uintptr_t>(hc);
    MLOG("combat: PROBE %s%s hp %.2f alive98 %02x dmg+a8 %llx %s| from%s", what, t_ours ? " (ours)" : "", hp,
         static_cast<unsigned>(mem::readOr<uint8_t>(h + 0x98)), static_cast<unsigned long long>(mem::readOr<uintptr_t>(h + 0xa8)), extra, trace);
}

void dumpDamageInfo(uintptr_t dmg, char* out, size_t cap) {
    out[0] = 0;
    if (!dmg || dmg < 0x10000) return;
    size_t len = 0;
    const uintptr_t base = mem::moduleBase();
    for (int i = 0; i < 32 && len + 24 < cap; ++i) {
        const uintptr_t q = mem::readOr<uintptr_t>(dmg + static_cast<uintptr_t>(i) * 8);
        len += static_cast<size_t>(snprintf(out + len, cap - len, i == 0 && q >= base ? "vt+%llx " : "%llx ",
                                            static_cast<unsigned long long>(i == 0 && q >= base ? q - base : q)));
    }
}

void hookSetHealth(void* hc, float v) {
    float now = -1.f;
    readHp(hc, now);
    if (v != now) { // the game also stores the unchanged value every few seconds: not logged
        char extra[16];
        snprintf(extra, sizeof(extra), "new %.2f ", v);
        probeLog("setHealth", hc, extra);
    }
    g_origSetHealth(hc, v);
}
uintptr_t hookApplyDamage(void* hc, uintptr_t result, uintptr_t dmg, uintptr_t a4) {
    char extra[1100] = "dmg: ";
    dumpDamageInfo(dmg, extra + 5, sizeof(extra) - 5);
    { // the damage source object the DamageInfo points at (+0xb0)
        const uintptr_t srcObj = mem::readOr<uintptr_t>(dmg + DamageInfo_Source);
        const size_t used = strlen(extra);
        snprintf(extra + used, sizeof(extra) - used, "| source %llx: ", static_cast<unsigned long long>(srcObj));
        if (srcObj > 0x10000 && strlen(extra) + 8 < sizeof(extra)) dumpDamageInfo(srcObj, extra + strlen(extra), sizeof(extra) - strlen(extra));
    }
    probeLog("applyDamage", hc, extra);
    const uintptr_t r = g_origApplyDamage(hc, result, dmg, a4);
    probeLog("applyDamage returned", hc, "");
    return r;
}
uintptr_t hookDie(void* hc, uintptr_t a, uintptr_t b) {
    probeLog("die", hc, "");
    return g_origDie(hc, a, b);
}

void installProbe() {
    if (g_probeInstalled) return;
    g_probeInstalled = true; // one try
    const uintptr_t vt = rva(Vtbl_PamServerHumanHealthComponent);
    uintptr_t cur[3] = {mem::readOr<uintptr_t>(vt + Vtbl_Health_SetHealth), mem::readOr<uintptr_t>(vt + kSlotApplyDamage),
                        mem::readOr<uintptr_t>(vt + kSlotDie)};
    for (uintptr_t f : cur)
        if (!inImage(f)) { MLOG("combat: death probe not installed: a slot is outside the image"); return; }
    g_origSetHealth = reinterpret_cast<SetHealthFn>(cur[0]);
    g_origApplyDamage = reinterpret_cast<Fn4>(cur[1]);
    g_origDie = reinterpret_cast<Fn3>(cur[2]);
    g_origSetHealthAddr = cur[0];
    g_origDieAddr = cur[2];
    const bool ok = mem::patchPointer(vt + Vtbl_Health_SetHealth, reinterpret_cast<uintptr_t>(&hookSetHealth), nullptr) &&
                    mem::patchPointer(vt + kSlotApplyDamage, reinterpret_cast<uintptr_t>(&hookApplyDamage), nullptr) &&
                    mem::patchPointer(vt + kSlotDie, reinterpret_cast<uintptr_t>(&hookDie), nullptr);
    MLOG("combat: death probe %s (human health slots: setHealth +%llx, applyDamage +%llx, die +%llx)", ok ? "installed" : "FAILED",
         static_cast<unsigned long long>(cur[0] - mem::moduleBase()), static_cast<unsigned long long>(cur[1] - mem::moduleBase()),
         static_cast<unsigned long long>(cur[2] - mem::moduleBase()));
}
#endif // MECMP_DEBUG_KEYS

// Clears the game's "just hurt" window (Health_HitWindowProp) so back-to-back hits of ours all count. Returns the
// flag as it was (-1 = the property could not be read).
int clearHitWindow(void* hc) {
    const uintptr_t prop = reinterpret_cast<uintptr_t>(hc) + Health_HitWindowProp;
    const auto value = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(rva(Fn_PropertyValue))(prop, rva(TI_BoolType));
    uint8_t was = 0;
    if (!value || !mem::read(value, was)) return -1;
    if (was) {
        const uint8_t zero = 0;
        mem::write(value, &zero, 1);
    }
    return was ? 1 : 0;
}

// The game's own damage on the player (see Fn_DamageCharacter): hp drops by `amount`, and at 0 or below the game
// runs its death (die(), the killed messages, the respawn) by itself.
// Laid out as the game builds one (AI coordinator 0x14392ee50, and natural hits read 2026-10-16): a damage source
// object (DamageSource_Size bytes, its constructor Fn_DamageSource_ctor, the damage type at +0x58) sits right before
// the DamageInfo, which points at it from +0xb0. die() reads the type from there (0x66 Melee, 0x67 Bullet, 0x68 Fall,
// 0x69 Field) to pick the death event the player's animation waits for; with no source the death froze. The type
// is also at +0xc0/+0xc8, flag bytes +0xa4 = 0x10, +0xbc/+0xbd = 1, ids -1 at +0xc4/+0xcc/+0xe0/+0xf0, the amount as an
// int at +0xe8 (1 at +0xec, as enemies send), the hit direction as a unit vector at +0x20 with the impulse (800 for a
// melee hit) at +0x2c, where it landed at +0x30 and where it came from at +0xd0. `from`/`to` (the striker's and our
// position) may be null: the direction is then left empty.
void gameDamage(void* hc, int amount, const float* from = nullptr, const float* to = nullptr, int32_t damageType = DamageType_Melee) {
    alignas(16) uint8_t block[DamageSource_Size + DamageInfo_Size] = {};
    alignas(16) uint8_t result[0x80] = {};
    uint8_t* src = block;
    uint8_t* dmg = block + DamageSource_Size;
    reinterpret_cast<void* (*)(void*)>(rva(Fn_DamageSource_ctor))(src);
    const int32_t type = damageType;
    memcpy(src + DamageSource_Type, &type, sizeof(type));
    const auto putF = [&](size_t off, float v) { memcpy(dmg + off, &v, sizeof(v)); };
    const auto putI = [&](size_t off, int32_t v) { memcpy(dmg + off, &v, sizeof(v)); };
    const uintptr_t vt = rva(Vtbl_PamServerDamageInfo);
    memcpy(dmg, &vt, sizeof(vt));
    const uintptr_t srcPtr = reinterpret_cast<uintptr_t>(src);
    memcpy(dmg + DamageInfo_Source, &srcPtr, sizeof(srcPtr));
    putF(0x88, 60.f);
    putI(0x90, -1);
    // (+0xa4 stays 0 as in natural hits; the bytes after it in the dumps are stack leftovers)
    dmg[0xbc] = 1;
    dmg[0xbd] = 1;
    putI(0xc0, type); putI(0xc4, -1); putI(0xc8, type); putI(0xcc, -1);
    putI(0xe0, -1); putI(0xe4, -1); putI(0xf0, -1); putI(0xf4, -1);
    putI(DamageInfo_AmountInt, amount);
    putI(0xec, 1);
    if (from && to) {
        float d[3] = {to[0] - from[0], 0.1f, to[2] - from[2]};
        if (type == DamageType_Fall) { d[0] = 0.f; d[1] = -1.f; d[2] = 0.f; } // a fall hits from above, as the game's own do
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 1e-3f) {
            for (int i = 0; i < 3; ++i) putF(0x20 + 4u * static_cast<size_t>(i), d[i] / len);
            putF(0x2c, 800.f);
            for (int i = 0; i < 3; ++i) putF(0x30 + 4u * static_cast<size_t>(i), to[i] + (i == 1 ? 1.0f : 0.f));
            for (int i = 0; i < 3; ++i) putF(0xd0 + 4u * static_cast<size_t>(i), from[i]);
        }
    }
#if MECMP_DEBUG_KEYS
    t_ours = true;
#endif
    reinterpret_cast<uintptr_t (*)(void*, void*, void*)>(rva(Fn_DamageCharacter))(hc, result, dmg);
#if MECMP_DEBUG_KEYS
    t_ours = false;
#endif
    reinterpret_cast<void (*)(void*)>(rva(Fn_DamageSource_dtor))(src); // releases what the game may have referenced
}

// A damage cry came from the game itself (a real hurt reaction): ours is then not needed.
std::atomic<DWORD> g_nativeCryAt{0};
DWORD g_cryCheckAt = 0;
const char* g_cryWave = nullptr;

// One hit on the local player, as `mode` allows. True when health was taken.
bool applyHit(const std::string& who, int mode, const char* how, const float* from = nullptr, const float* to = nullptr) {
    void* player = level::playerCharacter();
    void* hc = player ? healthOf(player) : nullptr;
    float hp = 0, maxHp = 0;
    if (!hc || !readHp(hc, hp, &maxHp)) { MLOG("combat: hit by %s ignored: no readable player health", who.c_str()); return false; }
    bool killed = false;
    const float next = strike::afterHit(hp, mode, &killed);
    if (next == hp) { MLOG("combat: hit by %s changes nothing (hp %.1f, mode %d)", who.c_str(), hp, mode); return false; }
    // Tried 2026-10-16 and dropped: leaving the game's hit window set for the killing blow (as in a natural fatal hit) did
    // not bring the death animation back and only made fatal hits get absorbed more often.
    const int window = clearHitWindow(hc);
    // (Tested 2026-10-16: a Fall-typed killing blow was no better than Melee. Filling the source in with a ghost as the
    // attacker through Fn_DamageSource_setAttacker crashed the game: a ghost's +0x14b8 is not an attacker handle.)
    gameDamage(hc, 1, from, to, DamageType_Melee);
    float after = -1.f;
    readHp(hc, after);
    if (!(after < hp)) { // the game took nothing: no hit, no notice, no cry
        MLOG("combat: hit by %s absorbed by the game (hp %.1f -> %.1f, hit window was %d)", who.c_str(), hp, after, window);
        return false;
    }
    MLOG("combat: %s hit us (%s): hp %.1f -> %.1f (expected %.1f, hit window was %d)%s", who.c_str(), how, hp, after, next, window,
         killed ? ", killed" : "");
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_notice = who + (killed ? " knocked you out" : " hit you");
    }
    // If the game did not cry out by itself within a moment, ours is heard here and, through the usual audio
    // packets, on our ghost at the others (player_audio.cpp).
    g_cryCheckAt = GetTickCount() + 400;
    g_cryWave = killed ? "ME_Sound/Character/Faith/Vocal/ME_Faith_Damage" : "ME_Sound/Character/Faith/Vocal/ME_Faith_DamageLite";
    g_nativeCryAt = 0;
    return true;
}

// A swing of the local player: who does it touch?
void decideStrike(const Strike& s) {
    const auto cfg = config::get();
    if (!cfg->dealHits || !net::running() || level::inFrontEnd()) return;
    if (g_haveStrike && !strike::elapsed(s.at, g_lastStrikeAt, strike::kSendCooldownMs)) return;
    static net::PlayerState me; // large; server thread only
    if (!net::localState(me)) return;
    strike::Candidate cand[16];
    size_t n = 0;
    bool loopbackId[16] = {};
    float nearest = 1e30f;
    uint32_t nearestId = 0;
    for (const net::RemoteInfo& r : net::remotes(1500)) {
        if (n >= 16) break;
        static net::PlayerState st;
        uint32_t age = 0;
        if (!net::remoteState(r.id, st, age) || age > 1000 || !(st.flags & net::kStateInLevel)) continue;
        const float dx = st.pos[0] - me.pos[0], dz = st.pos[2] - me.pos[2];
        const float d = dx * dx + dz * dz;
        if (d < nearest) { nearest = d; nearestId = r.id; }
        if (!r.allowsHits) continue; // only players who let others hurt them are candidates
        cand[n] = {r.id, {st.pos[0], st.pos[1], st.pos[2]}};
        loopbackId[n] = r.loopback;
        ++n;
    }
    const strike::Pose pose{{me.pos[0], me.pos[1], me.pos[2]}, me.yaw};
    const uint32_t target = strike::pickTarget(pose, s.kind, cand, n);
    VLOG("combat: swing (%s, %s kind %u) - %u candidates allow hits, nearest player %.2f m (id %08x) -> %s",
         sourceName(s.source), s.event ? "graph" : "vocal", s.kind, static_cast<unsigned>(n), nearest < 1e29f ? std::sqrt(nearest) : -1.f,
         nearestId, target ? "HIT" : "no hit");
    if (!target) return;
    g_lastStrikeAt = s.at;
    g_haveStrike = true;
    bool loopback = false;
    for (size_t i = 0; i < n; ++i)
        if (cand[i].id == target) loopback = loopbackId[i];
    if (loopback) { // the solo test: the mirrored ghost is us, so the hit comes back to us
        applyHit("Your mirror", cfg->hitsTaken, "solo test");
        return;
    }
    net::sendDamage(target, s.kind == strike::kKick ? proto::kDamageKick : proto::kDamagePunch);
}

// Hits other players sent us.
void receiveHits() {
    for (const net::DamageInfo& d : net::takeDamages()) {
        const auto cfg = config::get();
        const DWORD now = GetTickCount();
        const char* why = nullptr;
        static net::PlayerState me, st;
        uint32_t age = 0;
        if (cfg->hitsTaken == config::kHitsOff) why = "we do not allow hits";
        else if (level::inFrontEnd() || !level::playerCharacter()) why = "no level";
        else if (teleport::busy()) why = "teleporting";
        else if (net::inRun()) why = "in a run";
        else if (!net::localState(me)) why = "no own state";
        else if (!net::remoteState(d.from, st, age) || age > 1500 || !(st.flags & net::kStateInLevel)) why = "sender not in sight";
        else if (!strike::plausible({{st.pos[0], st.pos[1], st.pos[2]}, st.yaw}, d.kind, me.pos)) why = "sender too far";
        else if (!g_limiter.allow(d.from, now)) why = "cooldown";
        if (why) { MLOG("combat: hit from %s refused: %s", d.name.c_str(), why); continue; }
        applyHit(d.name, cfg->hitsTaken, "network", st.pos, me.pos);
    }
}

// Health watch and the developer test.
void watchHealth() {
    void* player = level::playerCharacter();
    void* hc = player ? healthOf(player) : nullptr;
    float hp = 0, maxHp = 0;
    if (!hc || !readHp(hc, hp, &maxHp)) { g_lastHealth = nullptr; g_lastHp = -1.f; return; }
    if (hc != g_lastHealth) {
        g_lastHealth = hc;
        MLOG("combat: player health component %p hp %.1f/%.1f", hc, hp, maxHp);
#if MECMP_DEBUG_KEYS
        { // the death events die() picks by damage type (data at hc+0x10, table at +0x80: {int type, u32 event hash}, type 100 = default)
            const uintptr_t data = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(hc) + 0x10);
            const uintptr_t table = data ? mem::readOr<uintptr_t>(data + 0x80) : 0;
            const uint32_t n = table ? mem::readOr<uint32_t>(table - 4) : 0;
            MLOG("combat: death event table: %u entries", n);
            for (uint32_t i = 0; i < n && i < 32; ++i)
                MLOG("combat:   damage type %d -> event %08x", mem::readOr<int32_t>(table + i * 8ull), mem::readOr<uint32_t>(table + i * 8ull + 4));
        }
#endif
    } else if (hp != g_lastHp) {
        VLOG("combat: player hp %.2f -> %.2f", g_lastHp, hp);
    }
    g_lastHp = hp;
#if MECMP_DEBUG_KEYS
    installProbe();
#endif
    if (g_cryWave && static_cast<int32_t>(GetTickCount() - g_cryCheckAt) >= 0) {
        if (g_nativeCryAt) MLOG("combat: the game cried out by itself, ours skipped");
        else {
            MLOG("combat: no cry from the game, playing ours");
            player_audio::requestOwnVocal(g_cryWave);
        }
        g_cryWave = nullptr;
    }
}
} // namespace

void onLocalCombatEvent(uint8_t source, uint32_t event) {
    static const int melee = indexOfSound("ME_Sound/Character/Faith/Melee/Faith_Combat_Melee");
    static const int kick = indexOfSound("ME_Sound/Character/Faith/Melee/Faith_Combat_DroneKick");
    static const int jab = indexOfVocal("ME_Sound/Character/Faith/Vocal/ME_Faith_PunchJab");
    static const int full = indexOfVocal("ME_Sound/Character/Faith/Vocal/ME_Faith_PunchFull");
    static const int hurt1 = indexOfVocal("ME_Sound/Character/Faith/Vocal/ME_Faith_Damage");
    static const int hurt2 = indexOfVocal("ME_Sound/Character/Faith/Vocal/ME_Faith_DamageLite");
    static const int hurt3 = indexOfSound("ME_Sound/Character/Faith/Melee/Faith_Damage_Impacts_Combat");
    if (source == hurt1 || source == hurt2 || source == hurt3) g_nativeCryAt = GetTickCount() | 1u;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_eventLogs < 400) { // what the player's own combat graphs do (which event is a swing?)
        ++g_eventLogs;
        VLOG("combat: local event %s hash %08x", sourceName(source), event);
    }
    if (source == kick) g_strikes.push_back({strike::kKick, source, event, GetTickCount()});
    else if (source == melee || source == jab || source == full) g_strikes.push_back({strike::kPunch, source, event, GetTickCount()});
    while (g_strikes.size() > 8) g_strikes.pop_front();
}

void tick() {
    std::deque<Strike> strikes;
    {
        std::lock_guard<std::mutex> lock(g_m);
        strikes.swap(g_strikes);
    }
    for (const Strike& s : strikes) decideStrike(s);
    receiveHits();
    watchHealth();
}

void levelChanged() {
    std::lock_guard<std::mutex> lock(g_m);
    g_strikes.clear();
    g_lastHealth = nullptr;
    g_lastHp = -1.f;
}

std::string takeNotice() {
    std::lock_guard<std::mutex> lock(g_m);
    std::string out;
    out.swap(g_notice);
    return out;
}


} // namespace mecmp::game::combat
