#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

#include "ui/gamepad.h"

#include <atomic>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "game/hooks.h"
#include "ui/overlay.h"
#include "ui/radial.h"

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
    std::atomic<uint16_t> wheelButtons{0}; // LB/RB pressed for the marker wheel: hidden until released
    std::atomic<uint8_t> triggers{0};      // bit 0 = LT, bit 1 = RT pulled past half (last read)
};
Pad g_pads[XUSER_MAX_COUNT];
std::atomic<bool> g_hideAccept{false}, g_hideDecline{false}; // the summon panel's buttons are hidden from the game
std::atomic<int> g_wheelPad{-1}; // the controller whose marker buttons are held (it drives the marker wheel)
std::atomic<int> g_wheelAction{static_cast<int>(Action::Ping)}; // which buttons it is: Ping or React
std::atomic<DWORD> g_fakePacket{1};

// Capture: 0 off, 1 waiting until nothing is held (the press that clicked "Change"), 2 waiting for
// a press, 3 collecting, 4 done.
std::atomic<int> g_capture{0};
std::atomic<uint16_t> g_captured{0};
std::atomic<int> g_captureOwner{-1}; // the controller whose press started the recording (-1: none yet)

struct Name { const char* name; uint16_t bit; };
const Name kNames[] = {
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"BACK", XINPUT_GAMEPAD_BACK}, {"START", XINPUT_GAMEPAD_START},
    {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
};

// The recording belongs to one controller: the first press after everything is let go on every
// controller is its choice, and the other controllers' buttons are ignored until it is released.
void track(int user, uint16_t now) {
    switch (g_capture.load()) {
    case 1: {
        bool anyHeld = false;
        for (const Pad& p : g_pads) anyHeld = anyHeld || p.held.load() != 0;
        if (!anyHeld) g_capture = 2;
        break;
    }
    case 2:
        if (now) { g_captureOwner = user; g_captured = now; g_capture = 3; }
        break;
    case 3:
        if (user != g_captureOwner.load()) break;
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
    if (r != ERROR_SUCCESS || !st || user >= XUSER_MAX_COUNT) {
        if (user < XUSER_MAX_COUNT) {
            // A controller that went away must not keep the marker wheel, nor the buttons it was holding.
            if (g_wheelPad.load() == static_cast<int>(user)) {
                g_wheelPad = -1;
                radial::cancel();
            }
            if (r != ERROR_SUCCESS) {
                g_pads[user].held = 0;
                g_pads[user].wheelButtons = 0;
            }
        }
        return r;
    }
    g_seen = true;
    Pad& pad = g_pads[user];
    const uint16_t now = st->Gamepad.wButtons;
    const uint16_t before = pad.held.exchange(now);
    uint16_t hidden = 0; // buttons the game must not see this time
    bool pingPressed = false, reactPressed = false, clearPressed = false, playersPressed = false;
    if (g_capture.load() != 0) track(static_cast<int>(user), now);
    for (int a = 0; a < kActions; ++a) {
        const uint16_t bind = g_binding[a].load();
        if (g_capture.load() == 0 && bind && (now & bind) == bind && (before & bind) != bind) {
            // The last button of the combination went down.
            if (a == static_cast<int>(Action::Menu)) {
                ui::toggleMenu();
                pad.swallow[a] = true;
            } else if (a == static_cast<int>(Action::Ping)) {
                if (!ui::menuOpen() && !game::hooks::gameMenuOpen()) {
                    pingPressed = true; // a short press places a marker, a long one opens the wheel (below)
                    pad.swallow[a] = true;
                }
            } else if (a == static_cast<int>(Action::React)) {
                if (!ui::menuOpen() && !game::hooks::gameMenuOpen()) {
                    reactPressed = true; // a short press sends the last answer, a long one opens its wheel
                    pad.swallow[a] = true;
                }
            } else if (a == static_cast<int>(Action::Players)) {
                if (!ui::menuOpen() && !game::hooks::gameMenuOpen()) {
                    playersPressed = true; // held: the players panel
                    pad.swallow[a] = true;
                }
            } else if (a == static_cast<int>(Action::PingClear) && g_wheelPad.load() == static_cast<int>(user) &&
                       radial::engaged()) {
                clearPressed = true; // only while the marker buttons are held; otherwise the game has these buttons
                pad.swallow[a] = true;
            }
        }
        if (pad.swallow[a] && !(now & bind)) pad.swallow[a] = false;
        if (pad.swallow[a]) hidden = static_cast<uint16_t>(hidden | bind);
    }
    // A summon waits (or "Go back?" shows): its buttons are the panel's, not the game's.
    // Not while a game menu is open: its buttons are the game's then.
    const bool summonHides = !game::hooks::gameMenuOpen();
    if (g_hideAccept.load() && summonHides) hidden = static_cast<uint16_t>(hidden | g_binding[static_cast<int>(Action::SummonAccept)].load());
    if (g_hideDecline.load() && summonHides) hidden = static_cast<uint16_t>(hidden | g_binding[static_cast<int>(Action::SummonDecline)].load());
    // The marker wheel: this pad's right stick points at a slot and LB/RB switch wheel while it is open.
    bool wheelOpen = false;
    const uint8_t trigNow = static_cast<uint8_t>((st->Gamepad.bLeftTrigger > 128 ? 1 : 0) | (st->Gamepad.bRightTrigger > 128 ? 2 : 0));
    const uint8_t trigWent = static_cast<uint8_t>(trigNow & ~pad.triggers.exchange(trigNow));
    bool panelOpen = false;
    if ((pingPressed || reactPressed || playersPressed) && g_wheelPad.load() < 0) {
        g_wheelPad = static_cast<int>(user);
        g_wheelAction = static_cast<int>(pingPressed ? Action::Ping : reactPressed ? Action::React : Action::Players);
    }
    if (g_wheelPad.load() == static_cast<int>(user)) {
        const int action = g_wheelAction.load();
        const uint16_t bind = g_binding[action].load();
        const bool held = bind && g_capture.load() == 0 && (now & bind) == bind;
        const uint16_t shoulders = static_cast<uint16_t>((XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER) & ~bind);
        const uint16_t went = static_cast<uint16_t>(now & ~before & shoulders);
        const bool players = action == static_cast<int>(Action::Players);
        const radial::Mode mode = action == static_cast<int>(Action::React) ? radial::Mode::React
                                  : players                                  ? radial::Mode::Players
                                                                             : radial::Mode::Markers;
        wheelOpen = radial::padUpdate(mode, pingPressed || reactPressed || playersPressed, held, st->Gamepad.sThumbRX / 32767.f,
                                      -st->Gamepad.sThumbRY / 32767.f,
                                      (went & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0, (went & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0,
                                      clearPressed, (trigWent & 2) != 0, (trigWent & 1) != 0);
        panelOpen = wheelOpen && players;
        if (wheelOpen) pad.wheelButtons = static_cast<uint16_t>(pad.wheelButtons | (now & shoulders));
        if (!held) g_wheelPad = -1;
    }
    // LB/RB that switched wheel stay hidden until they are released, also after the wheel closed.
    pad.wheelButtons = static_cast<uint16_t>(pad.wheelButtons & now);
    hidden = static_cast<uint16_t>(hidden | pad.wheelButtons);
    if (ui::menuOpen() || g_capture.load() != 0) {
        // The game sees a pad at rest. A new packet number each time, so it does not keep the values
        // it read before the menu opened (it skips packets whose number did not change).
        memset(&st->Gamepad, 0, sizeof(st->Gamepad));
        st->dwPacketNumber = 0x80000000u | g_fakePacket.fetch_add(1);
    } else if (hidden || wheelOpen) {
        st->Gamepad.wButtons = static_cast<uint16_t>(st->Gamepad.wButtons & ~hidden);
        if (wheelOpen) st->Gamepad.sThumbRX = st->Gamepad.sThumbRY = 0; // the camera stays still
        if (panelOpen) st->Gamepad.bLeftTrigger = st->Gamepad.bRightTrigger = 0; // the triggers are the panel's
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

std::string label(const std::string& button) {
    if (!config::get()->padPlayStation) return button;
    static const struct { const char* xbox; const char* ps; } kPs[] = {
        {"A", "Cross"}, {"B", "Circle"}, {"X", "Square"}, {"Y", "Triangle"}, {"LB", "L1"}, {"RB", "R1"},
        {"LS", "L3"},   {"RS", "R3"},    {"BACK", "Share"}, {"START", "Options"},
    };
    for (const auto& p : kPs)
        if (_stricmp(button.c_str(), p.xbox) == 0) return p.ps;
    return button; // the D-pad keeps its name
}

std::string labelCombo(const std::string& combo) {
    std::string out;
    size_t at = 0;
    while (at <= combo.size()) {
        size_t end = combo.find('+', at);
        if (end == std::string::npos) end = combo.size();
        std::string part = combo.substr(at, end - at);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) out += (out.empty() ? "" : "+") + label(part);
        at = end + 1;
    }
    return out.empty() ? "none" : out;
}

void setBinding(uint16_t buttons, Action action) {
    if (action != Action::Count) g_binding[static_cast<int>(action)] = buttons;
}
bool bindingFree(uint16_t buttons, Action action) {
    if (!buttons) return true;
    for (int a = 0; a < kActions; ++a)
        if (a != static_cast<int>(action) && g_binding[a].load() == buttons) return false;
    return true;
}
bool held(Action action) {
    if (action == Action::Count || g_capture.load() != 0) return false;
    const uint16_t bind = g_binding[static_cast<int>(action)].load();
    if (!bind) return false;
    for (const Pad& p : g_pads)
        if ((p.held.load() & bind) == bind) return true;
    return false;
}
void setSummonHidden(bool accept, bool decline) {
    g_hideAccept = accept;
    g_hideDecline = decline;
}
void beginCapture() { g_captured = 0; g_captureOwner = -1; g_capture = 1; }
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
