#include "game/ping_point.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "net/net.h"
#include "net/protocol.h"

namespace mecmp::game::ping {
namespace {
std::atomic<bool> g_requested{false};
}

void request() { g_requested = true; }

bool pickPoint(float out[3]) {
    static net::PlayerState local; // large; game thread only
    if (!net::localState(local)) return false;
    memcpy(out, local.pos, sizeof(local.pos));
    return true;
}

void tick() {
    if (!g_requested.exchange(false)) return;
    float pos[3];
    if (!pickPoint(pos)) {
        VLOG("ping: no position right now (not in a level?)");
        return;
    }
    uint8_t rgb[3] = {255, 210, 60};
    net::protocol::parseRgb(config::get()->pingColor, rgb);
    net::sendPing(pos, rgb);
    VLOG("ping: placed at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
}

} // namespace mecmp::game::ping
