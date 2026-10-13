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
#include "ui/glyph.h"
#include "ui/notices.h"
#include "ui/overlay.h"

namespace mecmp::ui::radial {
namespace {
constexpr int kSlots = config::kPingSlots;
constexpr float kPi = 3.14159265f;
// Sizes in units of the font size, so the wheel scales with the UI.
constexpr float kDeadZone = 2.0f, kInner = 3.0f, kOuter = 7.5f;
constexpr DWORD kPadSilentMs = 500; // a pad that stopped reporting has let go

// Idle -> Held (the key is down) -> Open (held long enough: the wheel shows) -> Idle on release.
// Used = something was placed while the key is still down (a digit, a click): wait for the release.
enum class State { Idle, Held, Open, Used };
enum class Source { Key, Pad };

std::mutex g_m; // guards everything below (window, render and input threads all come here)
State g_state = State::Idle;
Source g_source = Source::Key;
int g_key = 0;
DWORD g_downAt = 0, g_padAt = 0;
float g_x = 0.f, g_y = 0.f; // the pointer's offset from the middle, in pixels
int g_wheel = 0;
float g_unit = 18.f; // the font size of the last drawn frame
// Copies of the state for the threads that only ask.
std::atomic<bool> g_open{false}, g_engaged{false};

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

ImU32 rgba(const uint8_t rgb[3], int a) { return IM_COL32(rgb[0], rgb[1], rgb[2], a); }

// The functions below are called with g_m held.
void setState(State s) {
    g_state = s;
    g_open = s == State::Open;
    g_engaged = s == State::Held || s == State::Open;
}

void begin(Source source, int key) {
    const auto cfg = config::get();
    g_source = source;
    g_key = key;
    g_downAt = g_padAt = GetTickCount();
    g_x = g_y = 0.f;
    g_wheel = cfg->pingWheel >= 0 && cfg->pingWheel < cfg->wheelCount ? cfg->pingWheel : 0;
    setState(State::Held);
}

void promote(DWORD now) {
    if (g_state == State::Held && now - g_downAt >= kHoldMs) setState(State::Open);
}

// Places `slot` of the current wheel, and remembers it as the marker picked last. The slot goes to
// the request itself: the setting only changes a moment later (config::change is queued).
void place(int slot) {
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

int pointedSlot() { return game::radialSlot(g_x, g_y, g_unit * kDeadZone, kSlots); }

// The key or the buttons were let go.
void release() {
    promote(GetTickCount());
    if (g_state == State::Held) {
        game::ping::request(); // a short press
    } else if (g_state == State::Open) {
        const int slot = pointedSlot();
        if (slot >= 0) place(slot); // in the middle: nothing chosen
    }
    setState(State::Idle);
}

void stepWheel(int step) {
    if (g_state != State::Open || step == 0) return;
    const int count = std::clamp(config::get()->wheelCount, 1, config::kWheels);
    const int wheel = ((g_wheel + step) % count + count) % count;
    g_wheel = wheel;
    // Remembered for the next time the wheel opens; saved with the next setting that is.
    config::change([wheel](config::Settings& c) { c.pingWheel = wheel; }, false);
}

void clearNow() {
    if (g_state != State::Held && g_state != State::Open) return;
    net::clearPing();
    setState(State::Used);
}

// Render thread, every frame: the "clear markers" key (anywhere in the game) or the middle mouse
// button (while the wheel is open) held for kHoldMs removes all my markers, once per hold. The keys
// are asked for their state instead of followed through window messages, so keyboard keys and mouse
// buttons work the same way, with the wheel open (it swallows the mouse) or not.
void pollClear() {
    static bool held = false, done = false; // render thread only
    static DWORD since = 0;
    const int key = ui::pingClearVk();
    const bool now = ui::gameFocused() && !ui::menuOpen() && !chat::isOpen() && !game::hooks::gameMenuOpen() &&
                     ((key && down(key)) || (g_open.load() && down(VK_MBUTTON)));
    if (!now) {
        held = done = false;
        return;
    }
    if (!held) {
        held = true;
        since = GetTickCount();
        return;
    }
    if (done || GetTickCount() - since < kHoldMs) return;
    done = true;
    if (net::clearPing()) notices::push("Markers cleared");
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Held || g_state == State::Open) setState(State::Used); // the wheel closes, nothing is placed
}

void text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const char* s) {
    dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 220), s);
    dl->AddText(font, size, at, col, s);
}

void centred(ImDrawList* dl, ImFont* font, float size, ImVec2 mid, ImU32 col, const char* s) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, s);
    text(dl, font, size, ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), col, s);
}
} // namespace

bool open() { return g_open.load(); }
bool engaged() { return g_engaged.load(); }

void keyDown(int key) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Idle) begin(Source::Key, key);
}

void keyUp() {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Idle || g_source != Source::Key) return;
    if (g_state == State::Used) setState(State::Idle);
    else release();
}

void digit(int n) {
    std::lock_guard<std::mutex> lock(g_m);
    if ((g_state != State::Held && g_state != State::Open) || n < 1 || n > kSlots) return;
    place(n - 1);
    setState(State::Used);
}

void moved(float dx, float dy) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state != State::Open) return;
    float x = g_x + dx, y = g_y + dy;
    // The pointer stays a little beyond the ring, so turning round again is quick.
    const float maxLen = g_unit * kOuter * 1.15f;
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

void switchWheel(int step) {
    std::lock_guard<std::mutex> lock(g_m);
    stepWheel(step);
}

void cancel() {
    std::lock_guard<std::mutex> lock(g_m);
    setState(State::Idle);
}

bool padUpdate(bool pressed, bool held, float x, float y, bool prev, bool next, bool clear) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Idle) {
        if (!pressed || !held) return false;
        begin(Source::Pad, 0);
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
    g_x = std::clamp(x, -1.f, 1.f) * g_unit * kOuter;
    g_y = std::clamp(y, -1.f, 1.f) * g_unit * kOuter;
    if (prev) stepWheel(-1);
    if (next) stepWheel(1);
    return true;
}

void draw() {
    pollClear();
    float ox = 0.f, oy = 0.f;
    int wheel = 0, hot = -1;
    Source source = Source::Key;
    const float unit = ImGui::GetFontSize();
    {
        std::lock_guard<std::mutex> lock(g_m);
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
        ox = g_x;
        oy = g_y;
        wheel = g_wheel;
        source = g_source;
        hot = pointedSlot();
    }
    const auto cfg = config::get();
    const int count = std::clamp(cfg->wheelCount, 1, config::kWheels);
    if (wheel >= count) wheel = 0; // it was removed in the menu meanwhile
    const config::Wheel& slots = cfg->wheels[wheel];
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

    dl->AddCircleFilled(mid, rOut + unit * 0.35f, IM_COL32(0, 0, 0, 105), 72);
    const float step = 2.f * kPi / kSlots;
    const float gap = 0.035f;
    char num[4];
    for (int i = 0; i < kSlots; ++i) {
        const float centre = -kPi * 0.5f + i * step; // wedge 0 straight up, then clockwise
        const float a0 = centre - step * 0.5f + gap, a1 = centre + step * 0.5f - gap;
        const bool on = i == hot;
        dl->PathArcTo(mid, rOut, a0, a1, 14);
        dl->PathArcTo(mid, rIn, a1, a0, 10);
        dl->PathFillConcave(on ? rgba(rgb, 175) : IM_COL32(24, 24, 30, 205));
        dl->PathArcTo(mid, rOut, a0, a1, 14);
        dl->PathArcTo(mid, rIn, a1, a0, 10);
        dl->PathStroke(on ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 60), ImDrawFlags_Closed, on ? 2.5f : 1.f);

        const float dx = std::cos(centre), dy = std::sin(centre);
        // The slot's digit at the inner ring, its icon further out.
        snprintf(num, sizeof(num), "%d", i + 1);
        centred(dl, font, unit * 0.8f, ImVec2(mid.x + dx * (rIn + unit * 0.65f), mid.y + dy * (rIn + unit * 0.65f)),
                on ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 205, 220), num);
        const float rIcon = rIn + (rOut - rIn) * 0.6f;
        drawGlyph(dl, slots[i].glyph, ImVec2(mid.x + dx * rIcon, mid.y + dy * rIcon), unit * (on ? 1.15f : 0.95f),
                  on ? IM_COL32(255, 255, 255, 255) : rgba(rgb, 255), 1.f, rgba(accent, 255));
        if (wheel == tapWheel && i == tapSlot)
            dl->AddCircleFilled(ImVec2(mid.x + dx * (rOut - unit * 0.4f), mid.y + dy * (rOut - unit * 0.4f)), unit * 0.16f,
                                IM_COL32(255, 255, 255, 220));

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
        text(dl, font, size, at, on ? IM_COL32(255, 255, 255, 255) : IM_COL32(225, 225, 225, 235), title.c_str());
    }

    // Middle: the pointer and which wheel this is.
    dl->AddCircle(mid, unit * kDeadZone, IM_COL32(255, 255, 255, hot < 0 ? 140 : 40), 40, 1.f);
    dl->AddCircleFilled(ImVec2(mid.x + ox, mid.y + oy), unit * 0.3f, IM_COL32(255, 255, 255, 240), 16);
    dl->AddCircle(ImVec2(mid.x + ox, mid.y + oy), unit * 0.3f, IM_COL32(0, 0, 0, 220), 16, 1.5f);
    char name[32];
    snprintf(name, sizeof(name), "Wheel %d of %d", wheel + 1, count);
    centred(dl, font, unit * 1.15f, ImVec2(mid.x, mid.y - rOut - unit * 3.1f), IM_COL32(255, 255, 255, 240), name);
    const bool pad = source == Source::Pad;
    const char* what = hot < 0 ? "Release to cancel" : (pad ? "Release to place" : "Release / click to place");
    centred(dl, font, unit * 0.85f, ImVec2(mid.x, mid.y + rOut + unit * 2.3f), IM_COL32(255, 255, 255, 235), what);
    // The other controls, one to a line.
    std::string lines[3];
    int n = 0;
    if (pad) {
        lines[n++] = "LB / RB: switch wheel";
        if (!cfg->pingClearPad.empty()) lines[n++] = cfg->pingClearPad + ": clear markers";
    } else {
        lines[n++] = "Scroll: switch wheel";
        lines[n++] = "Tap 1-9 to place without aiming";
        // The middle button always clears here; the key too, unless it is that button or there is none.
        const bool key = !cfg->pingClearKey.empty() && _stricmp(cfg->pingClearKey.c_str(), "MOUSE3") != 0;
        lines[n++] = "Hold middle click" + (key ? " / " + ui::keyLabel(cfg->pingClearKey) : std::string()) + ": clear markers";
    }
    for (int i = 0; i < n; ++i)
        centred(dl, font, unit * 0.8f, ImVec2(mid.x, mid.y + rOut + unit * (3.4f + 1.05f * static_cast<float>(i))),
                IM_COL32(210, 210, 215, 220), lines[i].c_str());
}

} // namespace mecmp::ui::radial
