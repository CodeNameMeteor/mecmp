#include "game/raycast.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"

namespace mecmp::game::raycast {
namespace {
std::atomic<int> g_available{-1}; // -1 unchecked, 0 no, 1 yes

// What the caster reads (MOVAPS: 16-byte aligned). A radius above ~0 makes it a sphere sweep.
struct alignas(16) Query {
    float from[4] = {0, 0, 0, 0};
    float to[4] = {0, 0, 0, 0};
    float radius = 0.f;
    uint8_t flags[3] = {}; // set by the entity's query builder (0x143a22330); the caster does not read them
    uint8_t pad[9] = {};
};
static_assert(sizeof(Query) == 0x30 && offsetof(Query, radius) == 0x20, "Query layout");

// What the caster writes. A miss: position = to, fraction = 1, distance = |to - from|, hit = 0.
struct alignas(16) Result {
    float position[4] = {0, 0, 0, 0};
    float normal[4] = {0, 0, 0, 0};
    uint32_t id = 0;
    float fraction = 0.f;
    float distance = 0.f;
    uint8_t hit = 0;
    uint8_t pad2d[3] = {};
    uint16_t tail = 0; // always 0x0101
    uint8_t pad[0x0e] = {};
};
static_assert(sizeof(Result) == 0x40 && offsetof(Result, id) == 0x20 && offsetof(Result, hit) == 0x2c &&
              offsetof(Result, tail) == 0x30, "Result layout");

// Checked against the code: the caster's prologue (it keeps 0x1d0 bytes of locals).
bool checkFunction() {
    int a = g_available.load();
    if (a >= 0) return a == 1;
    static const uint8_t kExpect[] = {0x48, 0x89, 0xE0, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
                                      0xA8, 0x08, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xD0, 0x01, 0x00, 0x00};
    uint8_t got[sizeof(kExpect)] = {};
    const bool ok = mem::read(rva(Fn_RaycastQuery), got, sizeof(got)) && memcmp(got, kExpect, sizeof(got)) == 0;
    if (!ok) {
        char hex[sizeof(got) * 3 + 1] = {};
        for (size_t i = 0; i < sizeof(got); ++i) snprintf(hex + i * 3, 4, "%02X ", got[i]);
        MLOG("raycast: the ray cast function looks different in this build (%s); markers go where you stand", hex);
    }
    g_available = ok ? 1 : 0;
    return ok;
}

// The server world's physics, as the game's ray cast update finds it (0x143a1fd7a; the last three
// steps are the getter 0x1434ee940). Every object the cast then reads through is checked first, so
// a world that is going away gives a miss instead of a fault.
uintptr_t physics(uintptr_t world) {
    const uintptr_t manager = mem::readOr<uintptr_t>(world + World_PhysicsManager);
    const uintptr_t holder = manager ? mem::readOr<uintptr_t>(manager + PhysManager_Holder) : 0;
    const uintptr_t inner = holder ? mem::readOr<uintptr_t>(holder) : 0;
    const uintptr_t phys = inner ? mem::readOr<uintptr_t>(inner + PhysHolder_Physics) : 0;
    if (!phys || !inImage(mem::readOr<uintptr_t>(phys))) return 0;
    const uintptr_t havok = mem::readOr<uintptr_t>(phys + Phys_HavokWorld);
    const uintptr_t filterOwner = havok ? mem::readOr<uintptr_t>(havok + HavokWorld_FilterOwner) : 0;
    if (!filterOwner || !mem::readOr<uintptr_t>(filterOwner + FilterOwner_Filter)) return 0;
    return phys;
}

bool finite3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::fabs(v[0]) < 100000.f &&
           std::fabs(v[1]) < 100000.f && std::fabs(v[2]) < 100000.f;
}
} // namespace

bool available() { return checkFunction(); } // checked once (on the first call), then remembered

bool cast(const float from[3], const float to[3], Hit& out) {
    if (!checkFunction() || !finite3(from) || !finite3(to)) return false;
    const uintptr_t world = mem::readOr<uintptr_t>(rva(kServerGameWorld));
    const uintptr_t phys = world ? physics(world) : 0;
    if (!phys) return false;
    Query q;
    Result r;
    memcpy(q.from, from, sizeof(float) * 3);
    memcpy(q.to, to, sizeof(float) * 3);
    using Fn = void (*)(const Query*, void* world, void* phys, Result*);
    reinterpret_cast<Fn>(rva(Fn_RaycastQuery))(&q, reinterpret_cast<void*>(world), reinterpret_cast<void*>(phys), &r);
    if (r.hit != 1 || !finite3(r.position)) return false;
    memcpy(out.pos, r.position, sizeof(out.pos));
    memcpy(out.normal, r.normal, sizeof(out.normal));
    out.distance = r.distance;
    return true;
}

} // namespace mecmp::game::raycast
