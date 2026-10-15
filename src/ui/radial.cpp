#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/radial.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/config.h"
#include "game/camera_math.h"
#include "game/hooks.h"
#include "game/ping_point.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "ui/chat.h"
#include "ui/gamepad.h"
#include "ui/glyph.h"
#include "ui/markers.h"
#include "ui/notices.h"
#include "ui/overlay.h"

namespace mecmp::ui::radial {
namespace {
constexpr float kPi = 3.14159265f;
// Sizes in units of the font size, so the wheel scales with the UI.
constexpr float kDeadZone = 1.5f, kInner = 3.0f, kOuter = 7.5f;
// The pointer is kept this close to the middle, so turning to the opposite wedge is a short move;
// it is drawn stretched out to the ring (shownPointer), so the wheel looks as it did.
constexpr float kPointerMax = 3.75f;
// A pad's stick: a quarter of its travel is the dead zone; the rest reaches kPointerMax.
constexpr float kPadReach = kDeadZone / 0.25f;
constexpr DWORD kPadSilentMs = 500; // a pad that stopped reporting has let go
constexpr float kFadeInMs = 80.f;   // instant wheel: fades in, so a very short tap does not flash it

// Idle -> Held (the key is down) -> Open (held long enough: the wheel shows) -> Idle on release.
// Used = something was placed while the key is still down (a digit, a click): wait for the release.
// With the instant mode the key goes straight to Open.
enum class State { Idle, Held, Open, Used };
enum class Source { Key, Pad };

std::mutex g_m; // guards everything below (window, render and input threads all come here)
State g_state = State::Idle;
Source g_source = Source::Key;
Mode g_mode = Mode::Markers;
int g_key = 0;
DWORD g_downAt = 0, g_padAt = 0;
float g_x = 0.f, g_y = 0.f; // the pointer's offset from the middle, in pixels
int g_wheel = 0;
bool g_instant = false;     // the setting when this press began
float g_unit = 18.f; // the font size of the last drawn frame
markers::Focus g_target;    // React: the marker the answer goes to, fixed when the key went down
std::string g_note;         // a notice to show on the next frame (the render thread owns the notices)
// Copies of the state for the threads that only ask.
std::atomic<bool> g_open{false}, g_engaged{false}, g_reacting{false};

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

ImU32 rgba(const uint8_t rgb[3], int a) { return IM_COL32(rgb[0], rgb[1], rgb[2], a); }
ImU32 scaled(ImU32 col, float k) {
    const ImU32 a = static_cast<ImU32>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * k);
    return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// The functions below are called with g_m held.
void setState(State s) {
    g_state = s;
    g_open = s == State::Open;
    g_engaged = s == State::Held || s == State::Open;
    g_reacting = g_mode == Mode::React && g_engaged.load();
}

int slotCount() {
    const auto cfg = config::get();
    if (g_mode == Mode::React) return std::clamp(cfg->reactionSlots, config::kMinSlots, config::kPingSlots);
    const int wheel = std::clamp(g_wheel, 0, config::kWheels - 1);
    return std::clamp(cfg->wheelSlots[wheel], config::kMinSlots, config::kPingSlots);
}

void begin(Source source, int key, Mode mode) {
    const auto cfg = config::get();
    if (mode == Mode::React) {
        g_target = markers::focusedOther();
        if (!g_target.valid) { // nobody's marker under the crosshair: nothing to answer
            g_note = "Look at another player's marker to answer it";
            return;
        }
    }
    g_mode = mode;
    g_source = source;
    g_key = key;
    g_instant = cfg->wheelInstant;
    g_downAt = g_padAt = GetTickCount();
    g_x = g_y = 0.f;
    g_wheel = mode == Mode::Markers && cfg->pingWheel >= 0 && cfg->pingWheel < cfg->wheelCount ? cfg->pingWheel : 0;
    setState(g_instant ? State::Open : State::Held);
}

void promote(DWORD now) {
    if (g_state == State::Held && now - g_downAt >= kHoldMs) setState(State::Open);
}

// Sends the quick answer in `slot` to the marker's owner, and remembers it as the one picked last.
void answer(int slot) {
    const auto cfg = config::get();
    if (!g_target.valid || slot < 0 || slot >= cfg->reactionSlots) return;
    const std::string& text = cfg->reactions[slot].label;
    if (text.empty()) {
        g_note = "That answer has no text";
        return;
    }
    net::sendReact(g_target.owner, g_target.index, g_target.label, text);
    g_note = "You answered " + (g_target.name.empty() ? std::string("a marker") : g_target.name) + ": " + text;
    if (cfg->reactLast != slot) config::change([slot](config::Settings& c) { c.reactLast = slot; });
}

// Places `slot` of the current wheel, and remembers it as the marker picked last. The slot goes to
// the request itself: the setting only changes a moment later (config::change is queued).
void place(int slot) {
    if (g_mode == Mode::React) {
        answer(slot);
        return;
    }
    const int wheel = g_wheel;
    const auto cfg = config::get();
    if (cfg->pingLastWheel != wheel || cfg->pingLastSlot != slot || cfg->pingWheel != wheel)
        config::change([wheel, slot](config::Settings& c) {
            c.pingLastWheel = wheel;
            c.pingLastSlot = slot;
            c.pingWheel = wheel;
        });
    game::ping::request(wheel, slot);
}

// A short press: the marker picked last (or the one chosen for taps), or the last quick answer.
void tap() {
    if (g_mode == Mode::React) answer(config::get()->reactLast);
    else game::ping::request();
}

int pointedSlot() { return game::radialSlot(g_x, g_y, g_unit * kDeadZone, slotCount()); }

// The key or the buttons were let go.
void release() {
    const DWORD now = GetTickCount();
    promote(now);
    if (g_state == State::Held) {
        tap();
    } else if (g_state == State::Open) {
        const int slot = pointedSlot();
        if (slot >= 0) place(slot);                        // in the middle: nothing chosen...
        else if (g_instant && now - g_downAt < kHoldMs) tap(); // ...unless it was only a quick tap
    }
    setState(State::Idle);
}

void stepWheel(int step) {
    if (g_state != State::Open || step == 0 || g_mode != Mode::Markers) return;
    const int count = std::clamp(config::get()->wheelCount, 1, config::kWheels);
    const int wheel = ((g_wheel + step) % count + count) % count;
    g_wheel = wheel;
    // Remembered for the next time the wheel opens; saved with the next setting that is.
    config::change([wheel](config::Settings& c) { c.pingWheel = wheel; }, false);
}

void clearNow() {
    if (g_state != State::Held && g_state != State::Open) return;
    if (g_mode != Mode::Markers) return;
    net::clearPing();
    setState(State::Used);
}

// Where the pointer is drawn: as it is inside the dead zone, then stretched so that the pointer's
// farthest reach is the ring's outer edge.
ImVec2 shownPointer(float unit) {
    const float len = std::sqrt(g_x * g_x + g_y * g_y), dead = unit * kDeadZone;
    if (len <= dead || len <= 0.f) return ImVec2(g_x, g_y);
    const float reach = unit * kPointerMax, ring = unit * kOuter; // (not "far": windows.h defines it)
    const float t = std::min(1.f, (len - dead) / std::max(1.f, reach - dead));
    const float shown = dead + t * (ring - dead);
    return ImVec2(g_x * shown / len, g_y * shown / len);
}

// Render thread, every frame: the "clear markers" key (anywhere in the game) or the middle mouse
// button (while the wheel is open) held for kHoldMs removes all my markers, once per hold. Two quick
// taps of the key remove just the marker of mine the crosshair is on (at my feet in "where I stand"
// mode). The keys are asked for their state instead of followed through window messages, so keyboard
// keys and mouse buttons work the same way, with the wheel open (it swallows the mouse) or not.
constexpr DWORD kDoubleTapMs = 350; // the second press comes this soon after the first one let go

void removeFocused() {
    const markers::Focus own = markers::focusedOwn();
    if (!own.valid) {
        const markers::Focus other = markers::focusedOther();
        if (other.valid) // somebody else's: only its owner can take it away
            notices::push(other.name.empty() ? std::string("Can't remove another player's marker")
                                             : "Can't remove " + other.name + "'s marker");
        else
            notices::push("No marker of yours here");
        return;
    }
    net::removePing(own.index);
    notices::push(own.label.empty() ? std::string("Marker removed") : "Marker removed: " + own.label);
}

void pollClear() {
    static bool held = false, done = false, byKey = false; // render thread only
    static DWORD since = 0, lastTapAt = 0;
    const int key = ui::pingClearVk();
    const bool canAct = ui::gameFocused() && !ui::menuOpen() && !chat::isOpen() && !game::hooks::gameMenuOpen();
    const bool keyHeld = canAct && key && down(key);
    // Not on the answers wheel, and not when the middle button is the key that opened the wheel (or does
    // something else): holding it there must not clear.
    const bool wheelClear =
        canAct && g_open.load() && !g_reacting.load() && !ui::middleButtonIsABind() && down(VK_MBUTTON);
    const bool now = keyHeld || wheelClear;
    if (!now) {
        // A press that let go before the hold time is one tap of a possible double tap.
        if (held && !done && byKey && GetTickCount() - since < kHoldMs) lastTapAt = GetTickCount();
        held = done = false;
        return;
    }
    if (!held) {
        held = true;
        byKey = keyHeld;
        since = GetTickCount();
        // The second press of a double tap: it acts at once, and holding it on does not clear everything.
        if (keyHeld && !g_engaged.load() && lastTapAt && since - lastTapAt <= kDoubleTapMs) {
            lastTapAt = 0;
            done = true;
            removeFocused();
        }
        return;
    }
    if (done || GetTickCount() - since < kHoldMs) return;
    done = true;
    if (net::clearPing()) notices::push("Markers cleared");
    std::lock_guard<std::mutex> lock(g_m);
    if ((g_state == State::Held || g_state == State::Open) && g_mode == Mode::Markers)
        setState(State::Used); // the wheel closes, nothing is placed
}

void text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const char* s) {
    dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, static_cast<int>(220 * ((col >> IM_COL32_A_SHIFT & 0xFF) / 255.f))), s);
    dl->AddText(font, size, at, col, s);
}

void centred(ImDrawList* dl, ImFont* font, float size, ImVec2 mid, ImU32 col, const char* s) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, s);
    text(dl, font, size, ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), col, s);
}
} // namespace

bool open() { return g_open.load(); }
bool engaged() { return g_engaged.load(); }

void keyDown(int key, Mode mode) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Idle) begin(Source::Key, key, mode);
}

void keyUp(int key) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Idle || g_source != Source::Key || key != g_key) return;
    if (g_state == State::Used) setState(State::Idle);
    else release();
}

void digit(int n) {
    std::lock_guard<std::mutex> lock(g_m);
    if ((g_state != State::Held && g_state != State::Open) || n < 1 || n > slotCount()) return;
    place(n - 1);
    setState(State::Used);
}

void moved(float dx, float dy) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state != State::Open) return;
    const float sens = std::clamp(config::get()->wheelSensitivity, 0.1f, 3.f);
    float x = g_x + dx * sens, y = g_y + dy * sens;
    const float maxLen = g_unit * kPointerMax;
    const float len = std::sqrt(x * x + y * y);
    if (len > maxLen && len > 0.f) {
        x *= maxLen / len;
        y *= maxLen / len;
    }
    g_x = x;
    g_y = y;
}

void click() {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state != State::Open) return;
    const int slot = pointedSlot();
    if (slot >= 0) place(slot);
    setState(State::Used);
}

void dismiss() {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Open) setState(State::Used); // closed; the release then places nothing
}

void switchWheel(int step) {
    std::lock_guard<std::mutex> lock(g_m);
    stepWheel(step);
}

void cancel() {
    std::lock_guard<std::mutex> lock(g_m);
    setState(State::Idle);
}

bool padUpdate(Mode mode, bool pressed, bool held, float x, float y, bool prev, bool next, bool clear) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Idle) {
        if (!pressed || !held) return false;
        begin(Source::Pad, 0, mode);
        if (g_state == State::Idle) return false; // nothing to answer
    }
    if (g_source != Source::Pad) return false; // the keyboard has it
    const DWORD now = GetTickCount();
    g_padAt = now;
    if (!held) {
        if (g_state == State::Used) setState(State::Idle);
        else release();
        return false;
    }
    promote(now);
    if (clear) clearNow();
    if (g_state != State::Open) return false;
    float px = std::clamp(x, -1.f, 1.f) * g_unit * kPadReach, py = std::clamp(y, -1.f, 1.f) * g_unit * kPadReach;
    const float len = std::sqrt(px * px + py * py), maxLen = g_unit * kPointerMax;
    if (len > maxLen && len > 0.f) {
        px *= maxLen / len;
        py *= maxLen / len;
    }
    g_x = px;
    g_y = py;
    if (prev) stepWheel(-1);
    if (next) stepWheel(1);
    return true;
}

void draw() {
    pollClear();
    float ox = 0.f, oy = 0.f;
    int wheel = 0, hot = -1, n = 0;
    Source source = Source::Key;
    Mode mode = Mode::Markers;
    float fade = 1.f;
    std::string target;
    const float unit = ImGui::GetFontSize();
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_note.empty()) {
            notices::push(g_note);
            g_note.clear();
        }
        if (g_state == State::Idle) return;
        const DWORD now = GetTickCount();
        // The key release message can be missed (focus changes); the key state is the truth.
        if (g_source == Source::Key && !down(g_key)) {
            if (g_state == State::Used) setState(State::Idle);
            else release();
            return;
        }
        if (g_source == Source::Pad && now - g_padAt > kPadSilentMs) {
            setState(State::Idle);
            return;
        }
        g_unit = unit;
        promote(now);
        if (g_state != State::Open) return;
        const ImVec2 shown = shownPointer(unit);
        ox = shown.x;
        oy = shown.y;
        wheel = g_wheel;
        source = g_source;
        mode = g_mode;
        n = slotCount();
        hot = pointedSlot();
        if (g_instant) fade = std::clamp(static_cast<float>(now - g_downAt) / kFadeInMs, 0.f, 1.f);
        if (mode == Mode::React) {
            target = g_target.name.empty() ? std::string("a marker") : g_target.name;
            if (!g_target.label.empty()) target += ": " + g_target.label;
        }
    }
    const auto cfg = config::get();
    const int count = std::clamp(cfg->wheelCount, 1, config::kWheels);
    if (wheel >= count) wheel = 0; // it was removed in the menu meanwhile
    const config::Wheel& slots = mode == Mode::React ? cfg->reactions : cfg->wheels[wheel];
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 mid(screen.x * 0.5f, screen.y * 0.5f);
    const float rIn = unit * kInner, rOut = unit * kOuter;
    uint8_t rgb[3] = {255, 210, 60}, accent[3] = {255, 255, 255};
    net::protocol::parseRgb(cfg->pingColor, rgb);
    net::protocol::parseRgb(cfg->pingAccent, accent);
    // The slot a short press places, if it is on this wheel.
    const int tapWheel = cfg->pingTap ? cfg->pingFixedWheel : cfg->pingLastWheel;
    const int tapSlot = cfg->pingTap ? cfg->pingFixedSlot : cfg->pingLastSlot;
    const int markSlot = mode == Mode::React ? cfg->reactLast : (wheel == tapWheel ? tapSlot : -1);

    dl->AddCircleFilled(mid, rOut + unit * 0.35f, scaled(IM_COL32(0, 0, 0, 105), fade), 72);
    const float step = 2.f * kPi / static_cast<float>(n);
    const float gap = n > 1 ? std::min(0.035f, step * 0.2f) : 0.f;
    char num[4];
    for (int i = 0; i < n; ++i) {
        const float centre = -kPi * 0.5f + static_cast<float>(i) * step; // wedge 0 straight up, then clockwise
        const float a0 = centre - step * 0.5f + gap, a1 = centre + step * 0.5f - gap;
        const bool on = i == hot;
        const int segs = std::max(6, static_cast<int>(14.f * step / 0.70f));
        dl->PathArcTo(mid, rOut, a0, a1, segs);
        dl->PathArcTo(mid, rIn, a1, a0, segs);
        dl->PathFillConcave(scaled(on ? rgba(rgb, 175) : IM_COL32(24, 24, 30, 205), fade));
        dl->PathArcTo(mid, rOut, a0, a1, segs);
        dl->PathArcTo(mid, rIn, a1, a0, segs);
        dl->PathStroke(scaled(on ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 60), fade), ImDrawFlags_Closed, on ? 2.5f : 1.f);

        const float dx = std::cos(centre), dy = std::sin(centre);
        // The slot's digit at the inner ring, its icon further out.
        snprintf(num, sizeof(num), "%d", i + 1);
        centred(dl, font, unit * 0.8f, ImVec2(mid.x + dx * (rIn + unit * 0.65f), mid.y + dy * (rIn + unit * 0.65f)),
                scaled(on ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 205, 220), fade), num);
        const float rIcon = rIn + (rOut - rIn) * 0.6f;
        drawGlyph(dl, slots[i].glyph, ImVec2(mid.x + dx * rIcon, mid.y + dy * rIcon), unit * (on ? 1.15f : 0.95f),
                  on ? IM_COL32(255, 255, 255, 255) : rgba(rgb, 255), fade, rgba(accent, 255));
        if (i == markSlot)
            dl->AddCircleFilled(ImVec2(mid.x + dx * (rOut - unit * 0.4f), mid.y + dy * (rOut - unit * 0.4f)), unit * 0.16f,
                                scaled(IM_COL32(255, 255, 255, 220), fade));

        // The title outside the ring, on the side the wedge points to.
        const std::string& title = slots[i].label;
        if (title.empty()) continue;
        const float size = unit * (on ? 1.05f : 0.95f);
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, title.c_str());
        const float reach = rOut + unit * 0.7f;
        ImVec2 at(mid.x + dx * reach, mid.y + dy * reach);
        if (dx > 0.3f) at.y -= ts.y * 0.5f;                       // right side: starts at the ring
        else if (dx < -0.3f) { at.x -= ts.x; at.y -= ts.y * 0.5f; } // left side: ends at the ring
        else { at.x -= ts.x * 0.5f; if (dy < 0.f) at.y -= ts.y; }   // top and bottom: centred
        text(dl, font, size, at, scaled(on ? IM_COL32(255, 255, 255, 255) : IM_COL32(225, 225, 225, 235), fade), title.c_str());
    }

    // Middle: the pointer and which wheel this is.
    dl->AddCircle(mid, unit * kDeadZone, scaled(IM_COL32(255, 255, 255, hot < 0 ? 140 : 40), fade), 40, 1.f);
    dl->AddCircleFilled(ImVec2(mid.x + ox, mid.y + oy), unit * 0.3f, scaled(IM_COL32(255, 255, 255, 240), fade), 16);
    dl->AddCircle(ImVec2(mid.x + ox, mid.y + oy), unit * 0.3f, scaled(IM_COL32(0, 0, 0, 220), fade), 16, 1.5f);
    char name[128];
    if (mode == Mode::React) {
        snprintf(name, sizeof(name), "Answer %s", target.c_str());
    } else if (!cfg->wheelNames[wheel].empty()) {
        snprintf(name, sizeof(name), "%s", cfg->wheelNames[wheel].c_str());
    } else {
        snprintf(name, sizeof(name), "Wheel %d of %d", wheel + 1, count);
    }
    centred(dl, font, unit * 1.15f, ImVec2(mid.x, mid.y - rOut - unit * 3.1f), scaled(IM_COL32(255, 255, 255, 240), fade), name);
    const bool pad = source == Source::Pad;
    const char* verb = mode == Mode::React ? "send" : "place";
    char what[64];
    if (hot < 0) snprintf(what, sizeof(what), "Release to cancel");
    else snprintf(what, sizeof(what), pad ? "Release to %s" : "Release / click to %s", verb);
    centred(dl, font, unit * 0.85f, ImVec2(mid.x, mid.y + rOut + unit * 2.3f), scaled(IM_COL32(255, 255, 255, 235), fade), what);
    // The other controls, one to a line.
    std::string lines[4];
    int nl = 0;
    if (pad) {
        if (mode == Mode::Markers) {
            lines[nl++] = gamepad::label("LB") + " / " + gamepad::label("RB") + ": switch wheel";
            if (!cfg->pingClearPad.empty()) lines[nl++] = gamepad::labelCombo(cfg->pingClearPad) + ": clear markers";
        }
    } else {
        if (mode == Mode::Markers) {
            std::string sw = "Scroll";
            for (const std::string* k : {&cfg->wheelPrevKey, &cfg->wheelNextKey})
                if (!k->empty()) sw += " / " + ui::keyLabel(*k);
            lines[nl++] = sw + ": switch wheel";
        }
        lines[nl++] = std::string("Tap 1-") + std::to_string(n) + " to " + verb + " without aiming";
        lines[nl++] = "Right click: cancel";
        if (mode == Mode::Markers) {
            // The middle button clears here (unless it is bound to something else); the key too, unless it
            // is that button or there is none.
            const bool key = !cfg->pingClearKey.empty() && _stricmp(cfg->pingClearKey.c_str(), "MOUSE3") != 0;
            const bool middle = !ui::middleButtonIsABind();
            if (middle || key)
                lines[nl++] = std::string("Hold ") + (middle ? "middle click" : "") + (middle && key ? " / " : "") +
                              (key ? ui::keyLabel(cfg->pingClearKey) : std::string()) + ": clear markers";
        }
    }
    for (int i = 0; i < nl; ++i)
        centred(dl, font, unit * 0.8f, ImVec2(mid.x, mid.y + rOut + unit * (3.4f + 1.05f * static_cast<float>(i))),
                scaled(IM_COL32(210, 210, 215, 220), fade), lines[i].c_str());
}

} // namespace mecmp::ui::radial
