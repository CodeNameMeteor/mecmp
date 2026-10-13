#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

#include "ui/gamepad.h"

#include <atomic>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"
#include "game/ping_point.h"
#include "ui/overlay.h"

namespace mecmp::ui::gamepad {
namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
std::atomic<GetStateFn> g_orig{nullptr};
constexpr int kActions = static_cast<int>(Action::Count);
std::atomic<uint16_t> g_binding[kActions];
std::atomic<bool> g_seen{false};

// Per controller (XInput has 4): what was held last time, and for each action whether the game must
// be kept from seeing its buttons until they are released (they just triggered it).
struct Pad {
    std::atomic<uint16_t> held{0};
    std::atomic<bool> swallow[kActions];
};
Pad g_pads[XUSER_MAX_COUNT];
std::atomic<DWORD> g_fakePacket{1};

// Capture: 0 off, 1 waiting until nothing is held (the press that clicked "Change"), 2 waiting for
// a press, 3 collecting, 4 done.
std::atomic<int> g_capture{0};
std::atomic<uint16_t> g_captured{0};

struct Name { const char* name; uint16_t bit; };
const Name kNames[] = {
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"BACK", XINPUT_GAMEPAD_BACK}, {"START", XINPUT_GAMEPAD_START},
    {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
};

void track(uint16_t now) {
    switch (g_capture.load()) {
    case 1:
        if (!now) g_capture = 2;
        break;
    case 2:
        if (now) { g_captured = now; g_capture = 3; }
        break;
    case 3:
        if (now) g_captured = static_cast<uint16_t>(g_captured | now);
        else g_capture = 4;
        break;
    default:
        break;
    }
}

// Called by the game (its input thread) for each controller, every frame.
DWORD WINAPI getState(DWORD user, XINPUT_STATE* st) {
    const GetStateFn orig = g_orig.load();
    const DWORD r = orig ? orig(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS || !st || user >= XUSER_MAX_COUNT) return r;
    g_seen = true;
    Pad& pad = g_pads[user];
    const uint16_t now = st->Gamepad.wButtons;
    const uint16_t before = pad.held.exchange(now);
    uint16_t hidden = 0; // buttons the game must not see this time
    if (g_capture.load() != 0) track(now);
    for (int a = 0; a < kActions; ++a) {
        const uint16_t bind = g_binding[a].load();
        if (g_capture.load() == 0 && bind && (now & bind) == bind && (before & bind) != bind) {
            // The last button of the combination went down.
            if (a == static_cast<int>(Action::Menu)) {
                ui::toggleMenu();
                pad.swallow[a] = true;
            } else if (!ui::menuOpen()) {
                game::ping::request();
                pad.swallow[a] = true;
            }
        }
        if (pad.swallow[a] && !(now & bind)) pad.swallow[a] = false;
        if (pad.swallow[a]) hidden = static_cast<uint16_t>(hidden | bind);
    }
    if (ui::menuOpen() || g_capture.load() != 0) {
        // The game sees a pad at rest. A new packet number each time, so it does not keep the values
        // it read before the menu opened (it skips packets whose number did not change).
        memset(&st->Gamepad, 0, sizeof(st->Gamepad));
        st->dwPacketNumber = 0x80000000u | g_fakePacket.fetch_add(1);
    } else if (hidden) {
        st->Gamepad.wButtons = static_cast<uint16_t>(st->Gamepad.wButtons & ~hidden);
        st->dwPacketNumber = 0x80000000u | g_fakePacket.fetch_add(1);
    }
    return r;
}
} // namespace

void maintain() {
    const uintptr_t slot = game::rva(game::kXInputGetStatePtr);
    uintptr_t cur = 0;
    if (!mem::read(slot, cur) || !cur || cur == reinterpret_cast<uintptr_t>(&getState)) return;
    if (game::inImage(cur)) return; // not an XInput function: leave it alone
    g_orig = reinterpret_cast<GetStateFn>(cur);
    if (mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&getState), nullptr))
        MLOG("ui: gamepad hooked (menu buttons: %s)", format(g_binding[0].load()).c_str());
}

uint16_t parse(const std::string& combo) {
    uint16_t bits = 0;
    size_t at = 0;
    while (at <= combo.size()) {
        size_t end = combo.find('+', at);
        if (end == std::string::npos) end = combo.size();
        std::string part = combo.substr(at, end - at);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) {
            bool known = false;
            for (const Name& n : kNames)
                if (_stricmp(part.c_str(), n.name) == 0) { bits = static_cast<uint16_t>(bits | n.bit); known = true; }
            if (!known) return 0;
        }
        at = end + 1;
    }
    return bits;
}

std::string format(uint16_t buttons) {
    std::string s;
    for (const Name& n : kNames)
        if (buttons & n.bit) s += (s.empty() ? "" : "+") + std::string(n.name);
    return s.empty() ? "none" : s;
}

void setBinding(uint16_t buttons, Action action) {
    if (action != Action::Count) g_binding[static_cast<int>(action)] = buttons;
}
void beginCapture() { g_captured = 0; g_capture = 1; }
bool captureDone(uint16_t& buttons) {
    if (g_capture.load() != 4) return false;
    buttons = g_captured.load();
    g_capture = 0;
    return true;
}
void cancelCapture() { g_capture = 0; }
bool capturing() { return g_capture.load() != 0; }
bool active() { return g_seen.load(); }

} // namespace mecmp::ui::gamepad
