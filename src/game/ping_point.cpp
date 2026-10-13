#include "game/ping_point.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "game/camera.h"
#include "game/raycast.h"
#include "net/net.h"
#include "net/protocol.h"

namespace mecmp::game::ping {
namespace {
constexpr int kNone = -2, kTap = -1;
std::atomic<int> g_requested{kNone}; // kNone, kTap, or wheel * kPingSlots + slot
std::atomic<int> g_moveRequested{-1}; // a cell of mine, or -1
// Markers go out at most this often, so leaning on the key cannot flood the others.
constexpr int64_t kPauseMs = 300;
// An aimed marker: the ray starts this far in front of the camera and reaches this far (metres).
constexpr float kAimStart = 0.3f, kAimRange = 300.f;

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

void request(int wheel, int slot) {
    if (wheel < 0 || wheel >= config::kWheels || slot < 0 || slot >= config::kPingSlots) return;
    g_requested = wheel * config::kPingSlots + slot;
}

void request() { g_requested = kTap; }

void requestMove(int index) { g_moveRequested = index; }

bool pickPoint(float out[3]) {
    static net::PlayerState local; // large; game thread only
    if (!net::localState(local)) return false;
    memcpy(out, local.pos, sizeof(local.pos));
    CameraView cam;
    if (!config::get()->pingAim || !raycast::available() || !camera::read(cam)) return true;
    // The camera looks along minus its forward row (camera_math.h).
    float from[3], to[3];
    for (int i = 0; i < 3; ++i) {
        from[i] = cam.pos[i] - cam.forward[i] * kAimStart;
        to[i] = cam.pos[i] - cam.forward[i] * kAimRange;
    }
    raycast::Hit hit;
    if (!raycast::cast(from, to, hit)) {
        VLOG("ping: nothing within %.0f m of the camera at %.1f %.1f %.1f (looking %.2f %.2f %.2f); placed where you stand",
             kAimRange, cam.pos[0], cam.pos[1], cam.pos[2], -cam.forward[0], -cam.forward[1], -cam.forward[2]);
        return true;
    }
    VLOG("ping: aimed from %.1f %.1f %.1f, hit %.1f %.1f %.1f at %.2f m (normal %.2f %.2f %.2f)", from[0], from[1], from[2],
         hit.pos[0], hit.pos[1], hit.pos[2], hit.distance, hit.normal[0], hit.normal[1], hit.normal[2]);
    // Markers are drawn kPingLift above their position: an aimed one goes exactly where the ray hit.
    out[0] = hit.pos[0];
    out[1] = hit.pos[1] - net::protocol::kPingLift;
    out[2] = hit.pos[2];
    return true;
}

void tick() {
    static int lastMax = net::protocol::kPingCells; // game thread only
    static int64_t lastPlaced = -kPauseMs;
    const auto cfg = config::get();
    // Fewer markers allowed than before: the ones placed longest ago go.
    if (cfg->pingMax < lastMax) net::trimPings(cfg->pingMax);
    lastMax = cfg->pingMax;

    if (const int move = g_moveRequested.exchange(-1); move >= 0) {
        float to[3];
        if (pickPoint(to)) net::movePing(static_cast<uint8_t>(move), to);
    }

    if (g_requested.load() == kNone) return;
    const int64_t now = nowMs();
    if (now - lastPlaced < kPauseMs) return; // kept for a later tick
    const int what = g_requested.exchange(kNone);
    if (what == kNone) return;
    float pos[3];
    if (!pickPoint(pos)) {
        VLOG("ping: no position right now (not in a level?)");
        return;
    }
    int wheel = what / config::kPingSlots, slot = what % config::kPingSlots;
    if (what == kTap) {
        wheel = cfg->pingTap ? cfg->pingFixedWheel : cfg->pingLastWheel;
        slot = cfg->pingTap ? cfg->pingFixedSlot : cfg->pingLastSlot;
    }
    if (wheel < 0 || wheel >= cfg->wheelCount || wheel >= config::kWheels) wheel = slot = 0; // a wheel that was removed
    if (slot < 0 || slot >= config::kPingSlots) slot = 0;
    const config::WheelSlot& chosen = cfg->wheels[wheel][slot];
    net::PingStyle style;
    net::protocol::parseRgb(cfg->pingColor, style.rgb);
    net::protocol::parseRgb(cfg->pingAccent, style.accent);
    style.glyph = chosen.glyph;
    style.label = chosen.label;
    style.seconds = cfg->pingSeconds;
    net::sendPing(pos, style, cfg->pingMax);
    lastPlaced = now;
    VLOG("ping: wheel %d slot %d '%s' placed at %.1f %.1f %.1f", wheel + 1, slot + 1, chosen.label.c_str(), pos[0], pos[1], pos[2]);
}

} // namespace mecmp::game::ping
