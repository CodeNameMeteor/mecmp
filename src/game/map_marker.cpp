#include "game/map_marker.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/engine.h"

namespace mecmp::game::map_marker {
namespace {

constexpr uintptr_t kClientGameContext = 0x142401cb0; // *ptr + 0x10 = client message manager
constexpr uintptr_t kServerGameContext = 0x14253d670; // *ptr + 0x10 = server message manager
constexpr uintptr_t Fn_RegisterListener = 0x142a3ad40;

constexpr uint32_t kCategoryNetwork = 0xc67d277b;   // djb2-xor("Network")
constexpr uint32_t kTypeTrackerPlaced = 0x19500757; // djb2-xor("NetworkPamMapTrackerPlacedMessage")
constexpr size_t Msg_Category = 0x08, Msg_Type = 0x0c;
constexpr size_t Placed_Flag = 0x48, Placed_Pos = 0x80;

struct Listener {
    void* const* vtbl;
};
void onMessage(Listener* self, const void* message);
uint16_t priority(Listener*) { return 0; }
void* const g_vtbl[2] = {reinterpret_cast<void*>(&onMessage), reinterpret_cast<void*>(&priority)};
Listener g_clientListener{g_vtbl}, g_serverListener{g_vtbl};
std::atomic<uintptr_t> g_clientRegistered{0}, g_serverRegistered{0};

std::mutex g_m;
float g_pos[3] = {0, 0, 0};
bool g_have = false;

bool registerOn(uintptr_t contextGlobal, Listener& l, std::atomic<uintptr_t>& done) {
    const uintptr_t ctx = mem::readOr<uintptr_t>(rva(contextGlobal));
    if (!ctx) return false;
    const uintptr_t manager = ctx + 0x10;
    if (done.load() == manager) return true;
    using RegisterFn = char (*)(uintptr_t manager, uint32_t category, Listener* listener, uint32_t filter);
    reinterpret_cast<RegisterFn>(rva(Fn_RegisterListener))(manager, kCategoryNetwork, &l, 0xff);
    done = manager;
    VLOG("map: listening for the map marker on the %s message manager", &l == &g_clientListener ? "client" : "server");
    return true;
}

void onMessage(Listener* self, const void* message) {
    (void)self;
    const auto* m = static_cast<const uint8_t*>(message);
    uint32_t category = 0, type = 0;
    memcpy(&category, m + Msg_Category, 4);
    memcpy(&type, m + Msg_Type, 4);
    if (category != kCategoryNetwork || type != kTypeTrackerPlaced) return;
    float p[3];
    memcpy(p, m + Placed_Pos, sizeof(p));
    const uint8_t flag = m[Placed_Flag];
    for (float v : p)
        if (!std::isfinite(v) || std::fabs(v) > 100000.f) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_have && p[0] == g_pos[0] && p[1] == g_pos[1] && p[2] == g_pos[2]) return; // the other manager's copy
    memcpy(g_pos, p, sizeof(g_pos));
    g_have = true;
    MLOG("map: marker placed at %.1f %.1f %.1f (flag %u)", p[0], p[1], p[2], flag);
}

} // namespace

bool position(float out[3]) {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_have) return false;
    memcpy(out, g_pos, sizeof(g_pos));
    return true;
}

void clientTick() { registerOn(kClientGameContext, g_clientListener, g_clientRegistered); }
void serverTick() { registerOn(kServerGameContext, g_serverListener, g_serverRegistered); }

void levelChanged() {
    std::lock_guard<std::mutex> lock(g_m);
    g_have = false;
}

} // namespace mecmp::game::map_marker
