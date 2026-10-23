#include "../src/game/doors.cpp"
#include "../src/net/net.cpp"

#include <cstdio>

namespace gd = mecmp::game::doors;
namespace net = mecmp::net;
namespace mecmp::game::level {
uintptr_t serverWorld() {
    return 0;
}
std::string levelName() {
    return "test level";
}
bool inFrontEnd() {
    return false;
}
} // namespace mecmp::game::level
namespace mecmp::game::camera {
bool read(CameraView&) {
    return false;
}
} // namespace mecmp::game::camera
namespace mecmp::game::raycast {
bool cast(const float*, const float*, Hit&) {
    return false;
}
} // namespace mecmp::game::raycast

static int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::printf("FAILED line %d: %s\n", __LINE__, #x);                                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)
static unsigned writes = 0, reads = 0;
static bool closed = true;
static uintptr_t set(uintptr_t, uintptr_t, uint8_t) {
    ++writes;
    return 0x1234;
}
static uint8_t get(uintptr_t property, uintptr_t) {
    ++reads;
    return property == gd::g_closed && closed;
}
static void read(void*, void*, int* index, int*, void*) {
    ++*index;
}
static void put(void* base, size_t offset, uintptr_t value) {
    memcpy(static_cast<char*>(base) + offset, &value, sizeof(value));
}

int main() {
    alignas(16) char driver[256]{}, ant[512]{}, parent[32]{}, actor[128]{}, reader[256]{};
    alignas(16) char closedProperty[64]{}, pulseProperty[64]{};
    uintptr_t table[3]{}, holder = reinterpret_cast<uintptr_t>(ant) + 8;
    const auto managerSlot = mecmp::mem::rva(gd::kManager);
    const auto page = managerSlot & ~uintptr_t(65535);
    void* allocation = VirtualAlloc(reinterpret_cast<void*>(page), 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(allocation != nullptr);
    if (!allocation)
        return 1;
    put(reinterpret_cast<void*>(managerSlot), 0, 1);
    gd::g_manager = 1;
    gd::g_table = reinterpret_cast<uintptr_t>(table);
    gd::g_closedId = 1;
    gd::g_pulseId = 2;
    gd::g_closed = reinterpret_cast<uintptr_t>(closedProperty + 32);
    gd::g_pulse = reinterpret_cast<uintptr_t>(pulseProperty + 32);
    table[1] = reinterpret_cast<uintptr_t>(closedProperty);
    table[2] = reinterpret_cast<uintptr_t>(pulseProperty);
    put(closedProperty, 32, mecmp::mem::rva(gd::kBool));
    put(pulseProperty, 32, mecmp::mem::rva(gd::kBool));
    put(driver, 0, mecmp::mem::rva(gd::kDriver));
    put(driver, 0x28, 123);
    put(driver, 0x38, reinterpret_cast<uintptr_t>(ant) + 0x60);
    put(driver, 0x40, 456);
    put(ant, 0, mecmp::mem::rva(gd::kAnt));
    put(ant, 0x158, reinterpret_cast<uintptr_t>(parent));
    put(parent, 0x10, reinterpret_cast<uintptr_t>(actor));
    put(reader, 0, mecmp::mem::rva(gd::kReader));
    put(reader, 0x88, reinterpret_cast<uintptr_t>(&holder));
    uintptr_t binding[] = {gd::g_closed.load()};
    put(reader, 0x30, reinterpret_cast<uintptr_t>(binding));
    const float placement[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(driver + 0x90, placement, sizeof(placement));
    gd::Driver d;
    CHECK(gd::snapshot(reinterpret_cast<uintptr_t>(driver), d));
    gd::g_world = 1;
    gd::g_readyAt = 0;
    gd::indexDriver(d);
    gd::g_drivers.emplace(d.ant, d);
    gd::g_bankReady = true;
    gd::g_installed = true;
    gd::g_set = &set;
    gd::g_get = &get;
    gd::g_read = &read;
    net::g_run = true;
    net::g_role = net::Role::Host;
    net::g_session = 100;
    const float pos[3]{};
    net::publishDoorInterest(1, pos, true);
    auto& live = gd::g_drivers.at(d.ant);
    auto callback = [&](bool manual, bool remote) {
        gd::g_work[d.ant] = {false, manual, remote};
        gd::g_pendingReads = gd::g_work.size();
        int index = 0, property = 0;
        gd::readClosed(nullptr, reader + 0x30, &index, &property, actor + 0x40);
        CHECK(index == 1);
    };
    int index = 0, property = 0;
    gd::g_pendingReads = 0;
    gd::readClosed(nullptr, reader + 0x30, &index, &property, actor + 0x40);
    CHECK(reads == 0 && index == 1);
    net::g_myActivity.kind = net::protocol::kRunTimeTrial;
    callback(true, false);
    CHECK(writes == 0);
    callback(false, true);
    CHECK(writes == 0);
    net::g_myActivity.kind = net::protocol::kActivityNone;
    callback(true, false);
    CHECK(writes == 1 && !live.pending);
    callback(true, false);
    CHECK(writes == 1);
    closed = false;
    callback(false, false);
    CHECK(live.pending && live.application.done);
    live.pending = false;
    closed = true;
    live.application = {};
    callback(false, true);
    CHECK(writes == 1); // No shared state, so a queued command is stale.
    CHECK(gd::setBool(gd::g_pulse, reinterpret_cast<uintptr_t>(actor + 0x40), 1) == 0x1234);
    CHECK(writes == 2 && live.pending);
    live.pending = false;
    gd::g_ownWrite = true;
    gd::setBool(gd::g_pulse, reinterpret_cast<uintptr_t>(actor + 0x40), 1);
    CHECK(writes == 3 && !live.pending);
    gd::g_ownWrite = false;
    gd::setBool(gd::g_pulse, reinterpret_cast<uintptr_t>(actor + 0x40), 0);
    CHECK(writes == 4 && !live.pending);
    live.application = {};
    closed = true;
    for (unsigned i = 0; i < 5; ++i) {
        live.application.last = GetTickCount64() - 3001;
        callback(true, false);
    }
    CHECK(writes == 7 && live.application.exhausted());
    net::doors::Message shared;
    shared.op = net::doors::Op::State;
    shared.id = 1;
    shared.revision = 1;
    shared.key = live.key;
    net::g_doorClient.receive(shared, GetTickCount());
    CHECK(net::doorIsOpen(live.key));
    live.application = {};
    live.pending = false;
    live.manualPublish = false;
    mecmp::config::change([](auto& c) { c.syncDoors = false; });
    mecmp::config::applyPending();
    callback(false, true);
    CHECK(writes == 7);
    mecmp::config::change([](auto& c) { c.syncDoors = true; });
    mecmp::config::applyPending();
    net::g_myActivity.kind = net::protocol::kRunDash;
    callback(false, true);
    CHECK(writes == 7);
    net::g_myActivity.kind = net::protocol::kActivityNone;
    callback(false, true);
    CHECK(writes == 8 && !live.pending);
    closed = false;
    callback(false, false);
    CHECK(!live.pending);
    live.application = {};
    closed = true;
    net::g_doorClient.reset();
    callback(false, true);
    CHECK(writes == 8);
    gd::g_keys[live.key] = 2;
    gd::setBool(gd::g_pulse, reinterpret_cast<uintptr_t>(actor + 0x40), 1);
    CHECK(writes == 9 && !live.pending);
    live.manualPublish = true;
    live.application = {};
    live.application.attempted(GetTickCount64() - 3001);
    gd::g_request = true; // Bypass the 100 ms scheduler delay.
    gd::tick();
    CHECK(!live.manualPublish && gd::g_pendingReads == 0);
    gd::levelChanged();
    CHECK(gd::g_work.empty() && gd::g_pendingReads == 0 && gd::g_keys.empty());
    CHECK(gd::g_indexWorld == 0);
    VirtualFree(allocation, 0, MEM_RELEASE);
    net::g_run = false;
    std::printf("door adapter tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
