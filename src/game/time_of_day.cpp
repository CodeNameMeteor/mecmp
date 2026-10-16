#include "game/time_of_day.h"

#include <windows.h>

#include <atomic>
#include <cmath>

#include "core/config.h"
#include "core/log.h"
#include "core/mem.h"
#include "game/engine.h"
#include "game/level.h"
#include "net/net.h"
#include "net/protocol.h"

namespace mecmp::game::time_of_day {
namespace {
constexpr uintptr_t kClockUpdaters = 0x142598ac0;   // TimeOfDayClockUpdater* [2], by realm
constexpr uintptr_t Vtbl_ClockUpdater = 0x141cd82a0;
constexpr size_t Updater_Clock = 0x28, Clock_Time = 0x30;
constexpr float kDay = net::protocol::kDaySeconds;
constexpr float kTolerance = 5.f; // seconds apart before the host's time is written (a running clock drifts a little)
constexpr DWORD kEveryMs = 1000;

std::atomic<int> g_state{0}; // 0 off / nothing, 1 hosting, 2 following, 3 own time (switched off)

// The time field of a realm's clock, checked: the updater is what we expect and the value is a time of day.
uintptr_t timeField(int realm) {
    const uintptr_t updater = mem::readOr<uintptr_t>(rva(kClockUpdaters) + static_cast<uintptr_t>(realm) * 8);
    if (!updater || mem::readOr<uintptr_t>(updater) != rva(Vtbl_ClockUpdater)) return 0;
    const uintptr_t clock = mem::readOr<uintptr_t>(updater + Updater_Clock);
    if (!clock) return 0;
    float t = -1.f;
    if (!mem::read(clock + Clock_Time, t) || !std::isfinite(t) || t < 0.f || t > kDay) return 0;
    return clock + Clock_Time;
}

// Distance between two times of day, the short way round midnight.
float apart(float a, float b) {
    const float d = std::fabs(a - b);
    return d > kDay * 0.5f ? kDay - d : d;
}
} // namespace

bool current(float& seconds) {
    for (int realm : {1, 0}) { // 1 first: the clock the Cheat Engine path found
        if (const uintptr_t f = timeField(realm)) return mem::read(f, seconds);
    }
    return false;
}

void tick() {
    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    if (now - lastAt < kEveryMs) return;
    lastAt = now;
    const bool inLevel = level::serverWorld() && !level::inFrontEnd();
    float mine = -1.f;
    const bool haveMine = inLevel && current(mine);
    if (net::hosting()) {
        net::setTimeOfDay(haveMine ? mine : -1.f);
        g_state = haveMine ? 1 : 0;
        return;
    }
    net::setTimeOfDay(-1.f);
    float host = -1.f;
    if (!haveMine || !net::hostTimeOfDay(host)) {
        g_state = 0;
        return;
    }
    if (!config::get()->syncTimeOfDay) {
        g_state = 3;
        return;
    }
    g_state = 2;
    if (apart(host, mine) <= kTolerance) return;
    // Both realms' clocks (the server's drives the sky; a client clock, if there is one, follows the same time).
    bool wrote = false;
    for (int realm : {1, 0}) {
        if (const uintptr_t f = timeField(realm)) wrote = mem::write(f, &host, sizeof(host)) || wrote;
    }
    static float lastLogged = -1.f;
    if (wrote && (lastLogged < 0.f || apart(host, mine) > 60.f)) {
        MLOG("time of day: set to the host's %02d:%02d (was %02d:%02d)", static_cast<int>(host / 3600.f),
             static_cast<int>(std::fmod(host, 3600.f) / 60.f), static_cast<int>(mine / 3600.f),
             static_cast<int>(std::fmod(mine, 3600.f) / 60.f));
        lastLogged = host;
    }
}

const char* status() {
    switch (g_state.load()) {
    case 1: return "You host: the others see your time of day";
    case 2: return "Following the host's time of day";
    case 3: return "Your own time of day (the host's is not used)";
    default: return "Not in use (no player host with a time of day, or not in the city)";
    }
}

} // namespace mecmp::game::time_of_day
