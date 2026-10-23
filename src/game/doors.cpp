#include "game/doors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/mem.h"
#include "game/camera.h"
#include "game/door_policy.h"
#include "game/level.h"
#include "game/raycast.h"
#include "net/net.h"

namespace mecmp::game::doors {
namespace {
using Key = net::doors::Key;
using InitFn = uintptr_t (*)(void*);
using DestroyFn = uintptr_t (*)(void*, uint32_t);
using ReadFn = void (*)(void*, void*, int*, int*, void*);
using GetFn = uint8_t (*)(uintptr_t, uintptr_t);
using SetFn = uintptr_t (*)(uintptr_t, uintptr_t, uint8_t);
constexpr uintptr_t kDriver = 0x141c43850, kAnt = 0x141b06770, kReader = 0x141b06e50, kBool = 0x141a435c8;
constexpr uintptr_t kInitSlot = 0x141c438c8, kDestroySlot = 0x141c43888, kReadSlot = 0x141b7ad30,
                    kSetSlot = 0x141a435d0;
constexpr uintptr_t kInit = 0x143933cd0, kDestroy = 0x14392b200, kRead = 0x14342d510, kGet = 0x142c34a70,
                    kSet = 0x142c3ea40;
constexpr uintptr_t kManager = 0x1423dd0b8;
constexpr size_t kMaxDrivers = 16384;
struct Driver {
    uintptr_t address = 0, data = 0, ant = 0, mesh = 0, actor = 0;
    float placement[16]{};
    Key key;
    door_policy::Application application;
    uint64_t sampled = 0, publishUntil = 0;
    bool pending = false, closed = false, manualPublish = false;
};
std::mutex g_mutex;
std::map<uintptr_t, Driver> g_drivers; // Indexed by ANT identity, never sent over the network.
std::map<uintptr_t, uintptr_t> g_actors;
std::map<Key, unsigned> g_keys;
struct Work {
    bool query = false, manual = false, remote = false;
};
std::map<uintptr_t, Work> g_work;
std::atomic<size_t> g_pendingReads{0};
std::atomic<bool> g_inRun{false};
uintptr_t g_bootstrapWorld = 0;
uint64_t g_indexWorld = 0;
thread_local bool g_ownWrite = false;
std::atomic<bool> g_installed{false}, g_bankReady{false}, g_request{false};
std::atomic<uint64_t> g_world{0}, g_readyAt{0}, g_targetAt{0};
door_policy::Lifecycle g_lifecycle;
Key g_target;
Key g_requestedTarget;
uint64_t g_requestedAt = 0;
std::atomic<uintptr_t> g_manager{0}, g_table{0}, g_closed{0}, g_pulse{0};
uint32_t g_closedId = 0, g_pulseId = 0;
InitFn g_init = nullptr;
DestroyFn g_destroy = nullptr;
ReadFn g_read = nullptr;
GetFn g_get = nullptr;
SetFn g_set = nullptr;
uintptr_t q(uintptr_t at) {
    return mem::readOr<uintptr_t>(at);
}
bool snapshot(uintptr_t address, Driver& out) {
    if (q(address) != mem::rva(kDriver))
        return false;
    Driver d;
    d.address = address;
    d.data = q(address + 0x28);
    d.mesh = q(address + 0x40);
    const auto iface = q(address + 0x38);
    if (iface < 0x60 || !d.data || !d.mesh)
        return false;
    d.ant = iface - 0x60;
    if (q(d.ant) != mem::rva(kAnt) || !mem::read(address + 0x90, d.placement, sizeof(d.placement)))
        return false;
    Key key;
    if (!net::doors::makeKey(1, d.placement, key))
        return false;
    out = d;
    return true;
}
uintptr_t actorOf(const Driver& d) {
    const auto parent = q(d.ant + 0x158);
    const auto state = parent ? q(parent + 0x10) : 0;
    return state ? state + 0x40 : 0;
}
void indexDriver(Driver& d) {
    const auto world = g_world.load();
    if (world && net::doors::makeKey(world, d.placement, d.key))
        ++g_keys[d.key];
    d.actor = actorOf(d);
    if (d.actor) {
        const auto inserted = g_actors.emplace(d.actor, d.ant);
        if (!inserted.second && inserted.first->second != d.ant)
            inserted.first->second = 0;
    }
}
void eraseDriver(std::map<uintptr_t, Driver>::iterator it) {
    auto& d = it->second;
    const auto count = g_keys.find(d.key);
    if (count != g_keys.end() && !--count->second)
        g_keys.erase(count);
    const auto actor = g_actors.find(d.actor);
    if (actor != g_actors.end() && actor->second == d.ant)
        g_actors.erase(actor);
    g_work.erase(d.ant);
    g_pendingReads = g_work.size();
    g_drivers.erase(it);
}
uintptr_t initialized(void* self) {
    const auto result = g_init(self);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lifecycle.changed();
    Driver d;
    if (g_drivers.size() < kMaxDrivers && snapshot(reinterpret_cast<uintptr_t>(self), d)) {
        const auto old = g_drivers.find(d.ant);
        if (old != g_drivers.end())
            eraseDriver(old);
        indexDriver(d);
        g_drivers.emplace(d.ant, d);
    }
    return result;
}
uintptr_t destroyed(void* self, uint32_t flags) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lifecycle.beginDestroy();
        const auto address = reinterpret_cast<uintptr_t>(self);
        for (auto it = g_drivers.begin(); it != g_drivers.end();) {
            if (it->second.address == address)
                eraseDriver(it++);
            else
                ++it;
        }
    }
    const auto result = g_destroy(self, flags);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lifecycle.endDestroy();
    }
    return result;
}
bool current(const Driver& d, uintptr_t actor) {
    return q(d.address) == mem::rva(kDriver) && q(d.address + 0x28) == d.data && q(d.address + 0x38) == d.ant + 0x60 &&
           q(d.address + 0x40) == d.mesh && q(d.ant) == mem::rva(kAnt) && actorOf(d) == actor;
}
bool bankCurrent() {
    const auto closed = g_closed.load(), pulse = g_pulse.load();
    return g_bankReady && q(mem::rva(kManager)) == g_manager.load() &&
           q(g_table.load() + 8 * (g_closedId & 0x7ffff)) == closed - 0x20 &&
           q(g_table.load() + 8 * (g_pulseId & 0x7ffff)) == pulse - 0x20 && q(closed) == mem::rva(kBool) &&
           q(pulse) == mem::rva(kBool);
}
uintptr_t setBool(uintptr_t property, uintptr_t actor, uint8_t value) {
    const auto result = g_set(property, actor, value);
    if (!g_installed || !g_bankReady || property != g_pulse.load() || !value || g_ownWrite)
        return result;
    const auto world = g_world.load();
    if (!world || GetTickCount64() < g_readyAt.load())
        return result;
    const bool publish =
        door_policy::publishPulse(value, false, config::get()->syncDoors, net::inRun()) && net::running();
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto identity = g_actors.find(actor);
    if (identity == g_actors.end() || !identity->second)
        return result;
    const auto found = g_drivers.find(identity->second);
    if (found == g_drivers.end())
        return result;
    auto& d = found->second;
    if (world != g_world.load() || d.key.world != world || !current(d, actor) || !bankCurrent())
        return result;
    const auto count = g_keys.find(d.key);
    if (count == g_keys.end() || count->second != 1)
        return result;
    d.application.opened();
    d.closed = false;
    d.sampled = GetTickCount64();
    d.pending = publish;
    d.publishUntil = d.sampled + 5000;
    return result;
}
void readClosed(void* definition, void* bindings, int* assetIndex, int* propertyIndex, void* actorPtr) {
    bool relevant = false;
    if (g_pendingReads.load() && g_bankReady) {
        const auto index = *assetIndex;
        relevant = index >= 0 && index < 4096 && (*reinterpret_cast<uintptr_t**>(bindings))[index] == g_closed.load();
    }
    g_read(definition, bindings, assetIndex, propertyIndex, actorPtr);
    if (!relevant || !g_installed)
        return;
    const bool inRun = net::inRun();
    const bool sync = config::get()->syncDoors;
    const auto now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_world.load() || now < g_readyAt.load() || !bankCurrent())
        return;
    const auto reader = reinterpret_cast<uintptr_t>(bindings) - 0x30;
    if (q(reader) != mem::rva(kReader))
        return;
    const auto iface = q(q(reader + 0x88));
    if (iface < 8)
        return;
    const auto ant = iface - 8;
    const auto work = g_work.find(ant);
    const auto found = g_drivers.find(ant);
    if (work == g_work.end() || found == g_drivers.end())
        return;
    const auto request = work->second;
    g_work.erase(work);
    g_pendingReads = g_work.size();
    auto& d = found->second;
    const auto actor = reinterpret_cast<uintptr_t>(actorPtr);
    if (d.key.world != g_world.load() || !current(d, actor))
        return;
    const auto count = g_keys.find(d.key);
    if (count == g_keys.end() || count->second != 1)
        return;
    d.closed = g_get(g_closed.load(), actor) != 0;
    d.sampled = now;
    if (!d.closed) {
        d.application.opened();
        if (d.manualPublish && sync && !inRun) {
            d.pending = true;
            d.publishUntil = now + 5000;
        }
        d.manualPublish = false;
        return;
    }
    const bool remote = request.remote && sync && net::doorIsOpen(d.key);
    if (!door_policy::allowOpening(inRun) || (!request.manual && !remote) || !d.application.due(now))
        return;
    const auto pulse = g_pulse.load();
    if (g_get(pulse, actor) != 0)
        return;
    d.application.attempted(now);
    d.manualPublish |= request.manual && sync;
    g_ownWrite = true;
    g_set(pulse, actor, 1);
    g_ownWrite = false;
    if (d.application.exhausted())
        MLOG("doors: application limit reached for a door; no more pulses until it reloads");
}
bool code(uintptr_t va, const std::array<uint8_t, 16>& expected) {
    std::array<uint8_t, 16> bytes{};
    return mem::read(mem::rva(va), bytes.data(), bytes.size()) && bytes == expected;
}
void bank() {
    const auto manager = q(mem::rva(kManager));
    const auto count = mem::readOr<uint32_t>(manager + 0x80), exports = mem::readOr<uint32_t>(manager + 0xa0);
    if ((count != 33340 && count != 33341) || exports != 1357) {
        g_bankReady = false;
        return;
    }
    const auto table = q(manager + 0xa8), map = q(manager + 0xd0);
    std::vector<uint8_t> bytes(exports * 20);
    if (!table || !mem::read(map, bytes.data(), bytes.size())) {
        g_bankReady = false;
        return;
    }
    uint64_t hash = 14695981039346656037ULL;
    for (auto byte : bytes)
        hash = (hash ^ byte) * 1099511628211ULL;
    uint32_t closedId = 0, pulseId = 0;
    if (count == 33340 && hash == 0xd70756f58c02cb00ULL) {
        closedId = 0x85f3c;
        pulseId = 0x86256;
    } else if (count == 33341 && hash == 0xe20f1f8ac2e10d23ULL) {
        closedId = 0x85f77;
        pulseId = 0x86292;
    } else {
        g_bankReady = false;
        return;
    }
    const auto closed = q(table + 8 * (closedId & 0x7ffff)) + 0x20, pulse = q(table + 8 * (pulseId & 0x7ffff)) + 0x20;
    if (q(closed) != mem::rva(kBool) || q(pulse) != mem::rva(kBool)) {
        g_bankReady = false;
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_closedId = closedId;
    g_pulseId = pulseId;
    g_manager = manager;
    g_table = table;
    g_closed = closed;
    g_pulse = pulse;
    g_bankReady = true;
}
// One incremental late-injection pass. Keep the cursor between calls so a time
// slice or a lifecycle conflict cannot silently abandon the remaining heap.
std::atomic<int> g_scan{0}; // 0 waiting, 1 scanning, 2 complete, 3 limited
bool bootstrap() {
    static uintptr_t cursor = 0x10000;
    static uint64_t started = 0, bytesRead = 0;
    static unsigned added = 0, unreadable = 0;
    static std::vector<uint8_t> buffer(1024 * 1024);
    if (!started) {
        started = GetTickCount64();
        g_scan = 1;
    }
    const auto slice = GetTickCount64();
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    while (cursor < reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress)) {
        if (GetTickCount64() - started > 120000 || bytesRead > 32ULL * 1024 * 1024 * 1024) {
            g_scan = 3;
            MLOG("doors: bootstrap limited after %u drivers; some loaded doors may need a level reload", added);
            return true;
        }
        if (GetTickCount64() - slice >= 40)
            return false;
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &page, sizeof(page)) || !page.RegionSize) {
            g_scan = 3;
            MLOG("doors: bootstrap stopped at an unreadable memory map");
            return true;
        }
        const auto end = reinterpret_cast<uintptr_t>(page.BaseAddress) + page.RegionSize;
        if (end <= cursor) {
            g_scan = 3;
            return true;
        }
        const bool readable = page.State == MEM_COMMIT && page.Type == MEM_PRIVATE &&
                              !(page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                              (page.Protect & (PAGE_READWRITE | PAGE_WRITECOPY));
        if (!readable) {
            cursor = end;
            continue;
        }
        const size_t size = static_cast<size_t>(std::min<uintptr_t>(buffer.size(), end - cursor));
        uint64_t ticket;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_lifecycle.retiring)
                return false;
            ticket = g_lifecycle.ticket;
        }
        bool retry = false;
        if (mem::read(cursor, buffer.data(), size)) {
            for (size_t i = 0; i + 8 <= size; i += 8) {
                uintptr_t vt = 0;
                memcpy(&vt, buffer.data() + i, 8);
                if (vt != mem::rva(kDriver))
                    continue;
                Driver d;
                if (!snapshot(cursor + i, d))
                    continue;
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_lifecycle.accepts(ticket)) {
                    retry = true;
                    break;
                }
                if (g_drivers.size() < kMaxDrivers && !g_drivers.count(d.ant)) {
                    indexDriver(d);
                    g_drivers.emplace(d.ant, d);
                    ++added;
                }
            }
        } else
            ++unreadable;
        bytesRead += size;
        if (!retry)
            cursor += size;
    }
    g_scan = 2;
    MLOG("doors: bootstrap finished, %u drivers, %u unreadable chunks, %llu ms", added, unreadable,
         static_cast<unsigned long long>(GetTickCount64() - started));
    return true;
}
} // namespace

bool install() {
    if (g_installed)
        return true;
    if (!code(kRead,
              {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x41, 0x8b, 0x08, 0x48, 0x89, 0xd7}) ||
        !code(kInit, {0x48, 0x89, 0xe0, 0x57, 0x48, 0x81, 0xec, 0xa0, 0, 0, 0, 0x48, 0xc7, 0x40, 0x88, 0xfe}) ||
        !code(kDestroy,
              {0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}) ||
        !code(kGet, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb7, 0x41, 0x28, 0x48, 0x89}) ||
        !code(kSet, {0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xd0, 0x48, 0x8d, 0x51, 0x18, 0x41, 0x0f, 0xb6, 0xd8}) ||
        q(mem::rva(kInitSlot)) != mem::rva(kInit) || q(mem::rva(kDestroySlot)) != mem::rva(kDestroy) ||
        q(mem::rva(kReadSlot)) != mem::rva(kRead) || q(mem::rva(kSetSlot)) != mem::rva(kSet)) {
        MLOG("doors: unsupported code or an existing hook; door controls are off");
        return false;
    }
    g_init = reinterpret_cast<InitFn>(mem::rva(kInit));
    g_destroy = reinterpret_cast<DestroyFn>(mem::rva(kDestroy));
    g_read = reinterpret_cast<ReadFn>(mem::rva(kRead));
    g_get = reinterpret_cast<GetFn>(mem::rva(kGet));
    g_set = reinterpret_cast<SetFn>(mem::rva(kSet));
    const std::array<std::pair<uintptr_t, uintptr_t>, 4> hooks = {
        {{kDestroySlot, reinterpret_cast<uintptr_t>(&destroyed)},
         {kInitSlot, reinterpret_cast<uintptr_t>(&initialized)},
         {kReadSlot, reinterpret_cast<uintptr_t>(&readClosed)},
         {kSetSlot, reinterpret_cast<uintptr_t>(&setBool)}}};
    std::array<uintptr_t, 4> previous{};
    g_bootstrapWorld = level::serverWorld();
    for (size_t i = 0; i < hooks.size(); ++i) {
        if (!mem::patchPointer(mem::rva(hooks[i].first), hooks[i].second, &previous[i])) {
            while (i) {
                --i;
                mem::patchPointer(mem::rva(hooks[i].first), previous[i], nullptr);
            }
            return false;
        }
    }
    g_installed = true;
    MLOG("doors: lifecycle, open-pulse and pending-application callbacks installed");
    return true;
}

void levelChanged() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_world = 0;
    g_readyAt = GetTickCount64() + 2000;
    g_targetAt = 0;
    g_request = false;
    g_requestedAt = 0;
    g_work.clear();
    g_pendingReads = 0;
    g_keys.clear();
    g_actors.clear();
    g_indexWorld = 0;
    g_lifecycle.changed();
    for (auto& entry : g_drivers) {
        auto& d = entry.second;
        d.key = {};
        d.pending = false;
        d.manualPublish = false;
        d.application = {};
        d.sampled = 0;
    }
}

void tick() {
    if (!g_installed)
        return;
    const auto now = GetTickCount64();
    const auto world = net::doors::worldId(level::levelName());
    const bool inRun = net::inRun();
    const bool sync = config::get()->syncDoors && !inRun;
    g_inRun = inRun;
    g_world = world;
    CameraView camera{};
    const bool ready = world && !level::inFrontEnd() && camera::read(camera) && g_bankReady && now >= g_readyAt.load();
    const float zero[3] = {};
    net::publishDoorInterest(world, ready ? camera.pos : zero, sync && ready);
    static uint64_t checked = 0;
    if (now - checked < 100 && !g_request.load())
        return;
    checked = now;
    raycast::Hit hit{};
    float to[3];
    for (int i = 0; i < 3; ++i)
        to[i] = camera.pos[i] - 3.f * camera.forward[i];
    const bool haveHit = ready && !inRun && raycast::cast(camera.pos, to, hit);
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool requested = g_request.exchange(false) && !inRun && GetTickCount64() - g_requestedAt <= 300;
    const Key requestedTarget = g_requestedTarget;
    g_requestedAt = 0;
    if (g_indexWorld != world) {
        g_indexWorld = world;
        g_keys.clear();
        g_actors.clear();
        for (auto& entry : g_drivers) {
            entry.second.key = {};
            indexDriver(entry.second);
        }
    }
    g_work.clear();
    g_targetAt = 0;
    Driver* candidate = nullptr;
    bool ambiguous = false;
    for (auto& entry : g_drivers) {
        auto& d = entry.second;
        if (!d.actor) {
            d.actor = actorOf(d);
            if (d.actor) {
                const auto inserted = g_actors.emplace(d.actor, d.ant);
                if (!inserted.second && inserted.first->second != d.ant)
                    inserted.first->second = 0;
            }
        }
        if (d.pending) {
            if (!sync || !net::running() || now > d.publishUntil)
                d.pending = false;
            else if (net::publishDoorOpened(d.key))
                d.pending = false;
        }
        if (!sync || (d.manualPublish && now - d.application.last >= door_policy::Application::kRetryMs))
            d.manualPublish = false;
        if (!ready || d.key.world != world)
            continue;
        const auto count = g_keys.find(d.key);
        if (count == g_keys.end() || count->second != 1)
            continue;
        if (haveHit && door_policy::matchesHit(d.placement, camera.pos, hit.pos, hit.normal)) {
            if (candidate)
                ambiguous = true;
            candidate = &d;
        }
        float distance2 = 0;
        for (int i = 0; i < 3; ++i) {
            const float delta = d.placement[12 + i] - camera.pos[i];
            distance2 += delta * delta;
        }
        const bool nearby = distance2 <= net::doors::kLeaveRadius * net::doors::kLeaveRadius;
        const bool remote = sync && nearby && d.application.due(now) && net::doorIsOpen(d.key);
        if ((remote || d.manualPublish) && g_work.size() < 64)
            g_work[d.ant] = {false, false, remote};
    }
    if (candidate && !ambiguous) {
        auto& d = *candidate;
        if (!d.application.done && !d.application.exhausted()) {
            if (d.closed && d.sampled && now - d.sampled < 500) {
                g_target = d.key;
                g_targetAt = now;
                if (requested && d.key == requestedTarget)
                    g_work[d.ant] = {true, true, false};
            }
            if (!g_work.count(d.ant))
                g_work[d.ant] = {true, false, false};
        }
    }
    g_pendingReads = g_work.size();
}

void maintain() {
    if (!g_installed)
        return;
    static uint64_t checked = 0;
    static bool scanned = false;
    const auto now = GetTickCount64();
    if (now - checked >= 1000) {
        checked = now;
        bank();
    }
    const auto world = level::serverWorld();
    if (g_bootstrapWorld && world && world != g_bootstrapWorld)
        scanned = true;
    if (!scanned && door_policy::bootstrapNeeded(g_bootstrapWorld, world) && g_world.load() && !level::inFrontEnd() &&
        now >= g_readyAt.load() && g_bankReady)
        scanned = bootstrap();
}
bool canOpen() {
    const auto at = g_targetAt.load();
    return !g_inRun && at && GetTickCount64() - at < 250;
}
void requestOpen() {
    if (net::inRun())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!canOpen())
        return;
    g_requestedTarget = g_target;
    g_requestedAt = GetTickCount64();
    g_request = true;
}
std::string status() {
    if (!g_installed)
        return "Door controls are unavailable for this game build.";
    if (!g_bankReady)
        return "Waiting for a supported animation bank.";
    if (g_inRun)
        return "Door sync and the open button are paused during runs.";
    if (g_scan == 3)
        return "Door discovery was incomplete. Reload the level to register remaining doors.";
    if (g_scan == 1)
        return "Registering doors already loaded in this level.";
    if (!config::get()->syncDoors)
        return "Local only. Other players cannot change your doors.";
    return net::doorSyncAvailable() ? "Syncing nearby doors."
                                    : "Local controls ready. Waiting for door sync from the host.";
}
} // namespace mecmp::game::doors
