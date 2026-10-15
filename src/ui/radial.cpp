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
#include <vector>

#include "core/config.h"
#include "game/camera_math.h"
#include "game/hooks.h"
#include "game/ping_point.h"
#include "game/teleport.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "ui/chat.h"
#include "ui/gamepad.h"
#include "ui/glyph.h"
#include "ui/markers.h"
#include "ui/notices.h"
#include "ui/overlay.h"
#include "ui/summon.h"

namespace mecmp::ui::radial {
namespace {
constexpr float kPi = 3.14159265f;
// Sizes in units of the font size, so the wheel scales with the UI.
constexpr float kDeadZone = 1.5f, kInner = 3.0f, kOuter = 7.5f;
constexpr float kWheelScale = 1.15f; // the wheel is drawn this much bigger than the font size alone gives
// The pointer is kept this close to the middle, so turning to the opposite wedge is a short move;
// it is drawn stretched out to the ring (shownPointer), so the wheel looks as it did.
constexpr float kPointerMax = 3.75f;
// A pad's stick: a quarter of its travel is the dead zone; the rest reaches kPointerMax.
constexpr float kPadReach = kDeadZone / 0.25f;
constexpr DWORD kPadSilentMs = 500; // a pad that stopped reporting has let go
// The players panel on a pad: the stick's dead zone, and how far a full push moves the pointer per second
// (in font sizes; the panel is about 30 wide).
constexpr float kPadPanelDead = 0.2f, kPadPanelSpeed = 32.f;
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
// Copies of the state for the threads that only ask. g_marking = the markers wheel is up (not answers, not players).
std::atomic<bool> g_open{false}, g_engaged{false}, g_marking{false}, g_listing{false}; // listing = the players panel

// The players panel (Mode::Players). Its list is refreshed by the render thread every frame; the layout is
// the one drawn last, so the pointer is tested against what is on screen.
constexpr int kPerPage = 9;
struct Player {
    uint32_t id = 0;
    std::string name;
    bool live = false;  // heard from in the last second: Go to needs that
    bool inRun = false; // in a time trial or a dash
    bool asks = false;  // wants to be asked before Go to (VISIT)
};
struct Box {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f; // from the middle of the screen, in pixels
    bool has(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};
struct Layout {
    float halfW = 0.f, halfH = 0.f;
    int count = 0;           // players on this page
    Box buttons[kPerPage];
    bool host = false;       // may pull players in: the "Hard summon" tick box is shown
    Box hardBox, allBox;     // "Hard summon" (a tick box) and the summon button (everyone / picked / withdraw)
    float titleY = 0.f, gridY = 0.f, hintY = 0.f;
};
std::vector<Player> g_players; // in a stable order (session id)
int g_page = 0;
bool g_hard = false;           // "Hard summon" is ticked (for this opening only)
bool g_withdraw = false;       // our summon still waits for answers: the summon button withdraws it
std::vector<uint32_t> g_picked; // ticked players (session ids), for this opening only
bool g_picksGone = false;       // every picked player left: the next press of the button only says so
Layout g_layout;
// What a click asked for; done by the render thread (summon and its notices live there).
enum class Act { None, GoTo, SummonGroup, Withdraw };
struct Action {
    Act act = Act::None;
    uint32_t id = 0;
    std::string name;
    bool live = false, inRun = false, asks = false;
    std::vector<uint32_t> targets; // SummonGroup: empty = everyone
    bool hard = false;
};
Action g_action;

bool picked(uint32_t id) { return std::find(g_picked.begin(), g_picked.end(), id) != g_picked.end(); }

int pageCount() { return std::max(1, (static_cast<int>(g_players.size()) + kPerPage - 1) / kPerPage); }

// The panel's boxes for `count` players on the page; `unit` = the font size.
Layout layoutPanel(int count, bool host, float unit) {
    Layout l;
    l.count = count;
    l.host = host;
    const int cols = count <= 2 ? std::max(count, 1) : count == 4 ? 2 : 3;
    const int rows = std::max(1, (count + cols - 1) / cols);
    const float bw = unit * 8.5f, bh = unit * 2.7f, gap = unit * 0.6f, pad = unit * 1.1f;
    const float titleH = unit * 2.0f, hintH = unit * 2.0f, hostH = unit * 2.0f;
    const float gridW = cols * bw + (cols - 1) * gap, gridH = rows * bh + (rows - 1) * gap;
    const float w = std::max(gridW, unit * 19.f) + 2.f * pad;
    const float h = pad + titleH + gridH + gap + hostH + gap + hintH + pad;
    l.halfW = w * 0.5f;
    l.halfH = h * 0.5f;
    const float top = -l.halfH;
    l.titleY = top + pad + titleH * 0.4f;
    l.gridY = top + pad + titleH;
    for (int i = 0; i < count && i < kPerPage; ++i) {
        const int r = i / cols, c = i % cols;
        const int inRow = std::min(cols, count - r * cols); // a short last row is centred
        const float rowW = inRow * bw + (inRow - 1) * gap;
        const float x0 = -rowW * 0.5f + c * (bw + gap), y0 = l.gridY + r * (bh + gap);
        l.buttons[i] = {x0, y0, x0 + bw, y0 + bh};
    }
    float y = l.gridY + gridH + gap;
    // The summon row, for everyone: the tick box only for who may pull players in.
    if (host) {
        const float half = (w - 2.f * pad - gap) * 0.5f;
        l.hardBox = {-l.halfW + pad, y, -l.halfW + pad + half, y + hostH};
        l.allBox = {l.halfW - pad - half, y, l.halfW - pad, y + hostH};
    } else {
        l.allBox = {-l.halfW + pad, y, l.halfW - pad, y + hostH};
    }
    y += hostH + gap;
    l.hintY = y + hintH * 0.5f;
    return l;
}

// The player button under the pointer on this page (-1 = none).
int pointedPlayer() {
    for (int i = 0; i < g_layout.count; ++i)
        if (g_layout.buttons[i].has(g_x, g_y)) return i;
    return -1;
}

// Asks the render thread to act on page slot `i` (0..8 on this page).
void actOn(int i, Act act) {
    const size_t at = static_cast<size_t>(g_page) * kPerPage + static_cast<size_t>(i);
    if (i < 0 || at >= g_players.size()) return;
    const Player& p = g_players[at];
    Action a;
    a.act = act;
    a.id = p.id;
    a.name = p.name.empty() ? std::string("Runner") : p.name;
    a.live = p.live;
    a.inRun = p.inRun;
    a.asks = p.asks;
    g_action = std::move(a);
}

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
    g_marking = g_mode == Mode::Markers && g_engaged.load();
    g_listing = g_mode == Mode::Players && g_engaged.load();
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
    g_page = 0;
    g_hard = false;
    g_picked.clear();
    g_picksGone = false;
    g_layout = Layout{};
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
    uint8_t rgb[3], accent[3];
    config::markerColors(*cfg, cfg->reactions[slot], rgb, accent);
    net::sendReact(g_target.owner, g_target.index, g_target.label, text, cfg->reactions[slot].glyph, rgb, accent);
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
    if (g_mode == Mode::Players) { // letting go only closes the panel: never a teleport by accident
        setState(State::Idle);
        return;
    }
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
    if (g_state != State::Open || step == 0) return;
    if (g_mode == Mode::Players) {
        const int pages = pageCount();
        g_page = ((g_page + step) % pages + pages) % pages;
        return;
    }
    if (g_mode != Mode::Markers) return;
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
    // Not while the players panel is up: its keys are the panel's.
    const bool canAct = ui::gameFocused() && !ui::menuOpen() && !chat::isOpen() && !game::hooks::gameMenuOpen() && !g_listing.load();
    const bool keyHeld = canAct && key && down(key);
    // Not on the answers wheel, and not when the middle button is the key that opened the wheel (or does
    // something else): holding it there must not clear.
    const bool wheelClear =
        canAct && g_open.load() && g_marking.load() && !ui::middleButtonIsABind() && down(VK_MBUTTON);
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

// The players for the panel (render thread, without g_m: net has its own lock). The mirror of yourself
// (loopback) is listed only when it is all there is, for a test alone.
std::vector<Player> readPlayers() {
    const auto remotes = net::remotes(5000);
    const bool onlyMirror = std::all_of(remotes.begin(), remotes.end(), [](const net::RemoteInfo& r) { return r.loopback; });
    std::vector<Player> out;
    for (const net::RemoteInfo& r : remotes) {
        if (r.loopback && !onlyMirror) continue;
        Player p;
        p.id = r.id;
        p.name = r.name.empty() ? std::string("(joining...)") : r.name;
        if (r.loopback) p.name += " (mirror)";
        p.live = r.ageMs < 1000;
        p.inRun = r.runKind != net::protocol::kActivityNone;
        p.asks = r.asksVisit;
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Player& a, const Player& b) { return a.id < b.id; });
    return out;
}

// A Go to that waits for its player's run to end (render thread only). id 0 = none.
struct WaitGoTo {
    uint32_t id = 0;
    std::string name;
    DWORD since = 0;
};
WaitGoTo g_wait;
constexpr DWORD kWaitGoToMs = 15 * 60 * 1000; // a run longer than this: the wait is given up

// `asks`: they want to be asked first, so they are (summon::askVisit goes once they say yes).
void goNow(uint32_t id, const std::string& name, bool live, bool asks) {
    if (!game::teleport::available()) notices::push("Go to is not available in this game build");
    else if (game::teleport::busy()) notices::push("Already on the way");
    else if (!live) notices::push(name + " is not responding right now");
    else if (asks) summon::askVisit(id, name);
    else {
        game::teleport::requestGoTo(id);
        notices::push("Going to " + name);
    }
}

// What a click on the panel asked for (render thread).
void perform(const Action& a) {
    if (a.act == Act::None) return;
    if (net::inRun() && a.act != Act::Withdraw) { // our own run: no teleports in or out of it
        notices::push("You are in a run: finish or quit it first");
        return;
    }
    switch (a.act) {
    case Act::GoTo:
        if (g_wait.id == a.id) { // the same player again: the wait is called off
            g_wait = WaitGoTo{};
            notices::push("Go to " + a.name + " cancelled");
        } else if (a.inRun) {
            g_wait = {a.id, a.name, GetTickCount()};
            notices::push(a.name + " is in a run: you go to them when it is over (right-click them again to cancel)");
        } else {
            goNow(a.id, a.name, a.live, a.asks);
        }
        break;
    case Act::SummonGroup:
        if (summon::send(a.targets, a.hard, a.name) && a.inRun)
            notices::push(a.targets.empty() ? std::string("Players in a run can answer when it is over")
                                            : std::string("Picked players in a run can answer when it is over"));
        break;
    case Act::Withdraw:
        net::cancelSummon();
        notices::push("Summon withdrawn");
        break;
    case Act::None: break;
    }
}

// The waiting Go to: goes once the player's run is over; dropped if they leave, or after kWaitGoToMs.
void tickWait() {
    if (!g_wait.id) return;
    if (GetTickCount() - g_wait.since > kWaitGoToMs) {
        notices::push("Stopped waiting for " + g_wait.name);
        g_wait = WaitGoTo{};
        return;
    }
    for (const net::RemoteInfo& r : net::remotes(5000)) {
        if (r.id != g_wait.id) continue;
        if (r.runKind != net::protocol::kActivityNone) return; // still running
        if (net::inRun()) return;                               // we started one meanwhile: wait for ours too
        const WaitGoTo w = g_wait;
        g_wait = WaitGoTo{};
        goNow(w.id, w.name, r.ageMs < 1000, r.asksVisit);
        return;
    }
    notices::push(g_wait.name + " left: Go to cancelled");
    g_wait = WaitGoTo{};
}

// While a Go to waits: a line at the top says for whom.
void drawWait(ImDrawList* dl, ImFont* font, float unit) {
    if (!g_wait.id || ui::overlayHidden()) return;
    const std::string line = "Going to " + g_wait.name + " when their run is over";
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    centred(dl, font, unit * 0.75f, ImVec2(screen.x * 0.5f, screen.y * 0.1f), IM_COL32(255, 255, 255, 210), line.c_str());
}

void drawPanel(ImDrawList* dl, ImFont* font, float unit, ImVec2 mid, const Layout& l, const std::vector<Player>& page,
               int pageNo, int pages, int hot, bool hard, ImVec2 pointer, float fade, bool pad,
               const std::vector<uint32_t>& pickedIds, bool withdraw) {
    auto tickBox = [&](ImVec2 at, float s, bool on, bool hot) { // a tick box with its top-left corner at `at`
        dl->AddRectFilled(at, ImVec2(at.x + s, at.y + s), scaled(IM_COL32(10, 10, 14, hot ? 230 : 170), fade), unit * 0.15f);
        dl->AddRect(at, ImVec2(at.x + s, at.y + s), scaled(IM_COL32(255, 255, 255, hot ? 255 : 200), fade), unit * 0.15f, 0, 1.5f);
        if (on) dl->AddRectFilled(ImVec2(at.x + s * 0.22f, at.y + s * 0.22f), ImVec2(at.x + s * 0.78f, at.y + s * 0.78f),
                                  scaled(IM_COL32(255, 110, 70, 255), fade), unit * 0.08f);
    };
    auto box = [&](const Box& b) { return std::make_pair(ImVec2(mid.x + b.x0, mid.y + b.y0), ImVec2(mid.x + b.x1, mid.y + b.y1)); };
    dl->AddRectFilled(ImVec2(mid.x - l.halfW, mid.y - l.halfH), ImVec2(mid.x + l.halfW, mid.y + l.halfH),
                      scaled(IM_COL32(8, 8, 12, 175), fade), unit * 0.6f);
    char title[48];
    if (pages > 1) snprintf(title, sizeof(title), "Players  %d / %d", pageNo + 1, pages);
    else snprintf(title, sizeof(title), "Players");
    centred(dl, font, unit * 1.15f, ImVec2(mid.x, mid.y + l.titleY), scaled(IM_COL32(255, 255, 255, 240), fade), title);
    if (page.empty())
        centred(dl, font, unit * 0.95f, ImVec2(mid.x, mid.y + l.gridY + unit * 1.35f), scaled(IM_COL32(200, 200, 205, 220), fade),
                "Nobody else here yet");
    char num[4];
    for (int i = 0; i < l.count && i < static_cast<int>(page.size()); ++i) {
        const Player& p = page[static_cast<size_t>(i)];
        const auto [a, b] = box(l.buttons[i]);
        const bool on = i == hot;
        const bool sel = std::find(pickedIds.begin(), pickedIds.end(), p.id) != pickedIds.end(); // picked for a summon
        const int alpha = p.live ? 255 : 130; // not heard from: dimmed, Go to will refuse
        // A plate under each name, so it never melts into the world behind it: red when picked, a little
        // lighter under the pointer.
        const ImU32 plate = sel  ? IM_COL32(232, 50, 46, p.live ? (on ? 235 : 210) : 120)
                            : on ? IM_COL32(58, 58, 70, p.live ? 240 : 160)
                                 : IM_COL32(28, 28, 36, p.live ? 230 : 150);
        dl->AddRectFilled(a, b, scaled(plate, fade), unit * 0.35f);
        dl->AddRect(a, b, scaled(on ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 50), fade), unit * 0.35f, 0, on ? 2.f : 1.f);
        const float cx = (a.x + b.x) * 0.5f;
        std::string name = p.name;
        const float maxW = (b.x - a.x) - unit * 0.8f;
        while (name.size() > 4 && font->CalcTextSizeA(unit * 1.05f, FLT_MAX, 0.f, name.c_str()).x > maxW)
            name.erase(name.size() - 4) += "...";
        centred(dl, font, unit * 1.05f, ImVec2(cx, a.y + (b.y - a.y) * 0.36f), scaled(IM_COL32(255, 255, 255, alpha), fade), name.c_str());
        snprintf(num, sizeof(num), "%d", i + 1);
        centred(dl, font, unit * 0.8f, ImVec2(cx, a.y + (b.y - a.y) * 0.76f), scaled(IM_COL32(210, 210, 215, alpha * 9 / 10), fade), num);
        if (p.inRun) { // a small tag at the plate's corner
            const char* tag = "in a run";
            const ImVec2 ts = font->CalcTextSizeA(unit * 0.7f, FLT_MAX, 0.f, tag);
            text(dl, font, unit * 0.7f, ImVec2(b.x - ts.x - unit * 0.3f, b.y - ts.y - unit * 0.15f),
                 scaled(IM_COL32(255, 200, 90, alpha), fade), tag);
        }
    }
    // The summon row, for everyone. The tick box (who may pull players in) makes the button a hard summon.
    if (l.host) {
        const auto [ha, hb] = box(l.hardBox);
        const bool hardHot = l.hardBox.has(pointer.x, pointer.y);
        dl->AddRectFilled(ha, hb, scaled(IM_COL32(28, 28, 36, hardHot ? 245 : 210), fade), unit * 0.3f);
        const float s = unit * 0.9f, bx = ha.x + unit * 0.5f, by = (ha.y + hb.y) * 0.5f - s * 0.5f;
        tickBox(ImVec2(bx, by), s, hard, hardHot);
        const ImVec2 ts = font->CalcTextSizeA(unit * 0.9f, FLT_MAX, 0.f, "Hard summon");
        text(dl, font, unit * 0.9f, ImVec2(bx + s + unit * 0.45f, (ha.y + hb.y) * 0.5f - ts.y * 0.5f),
             scaled(IM_COL32(255, 255, 255, 235), fade), "Hard summon");
    }
    {
        const auto [aa, ab] = box(l.allBox);
        const bool allHot = l.allBox.has(pointer.x, pointer.y);
        const bool hardNow = hard && l.host && !withdraw;
        const ImU32 base = withdraw ? IM_COL32(60, 60, 72, 220) : hardNow ? IM_COL32(120, 45, 30, 220) : IM_COL32(30, 90, 60, 220);
        const ImU32 lit = withdraw ? IM_COL32(110, 110, 125, 220) : hardNow ? IM_COL32(255, 110, 70, 210) : IM_COL32(60, 180, 110, 210);
        dl->AddRectFilled(aa, ab, scaled(allHot ? lit : base, fade), unit * 0.3f);
        dl->AddRect(aa, ab, scaled(IM_COL32(255, 255, 255, allHot ? 235 : 60), fade), unit * 0.3f, 0, allHot ? 2.f : 1.f);
        char what[48];
        if (withdraw) snprintf(what, sizeof(what), "Withdraw summon");
        else if (pickedIds.empty()) snprintf(what, sizeof(what), hardNow ? "Pull in everyone" : "Summon everyone");
        else snprintf(what, sizeof(what), hardNow ? "Pull in picked (%d)" : "Summon picked (%d)", static_cast<int>(pickedIds.size()));
        centred(dl, font, unit * 0.9f, ImVec2((aa.x + ab.x) * 0.5f, (aa.y + ab.y) * 0.5f), scaled(IM_COL32(255, 255, 255, 255), fade),
                what);
    }
    std::string sw;
    const auto cfg = config::get();
    for (const std::string* k : {&cfg->wheelPrevKey, &cfg->wheelNextKey})
        if (!k->empty()) sw += (sw.empty() ? "" : " / ") + ui::keyLabel(*k);
    char hint[160];
    if (pad)
        snprintf(hint, sizeof(hint), "RT: pick    LT: go to%s", pages > 1 ? ("    " + gamepad::label("LB") + ": page").c_str() : "");
    else
        snprintf(hint, sizeof(hint), "Click: pick for summon    Right click: go to    1-9: go to%s%s", pages > 1 && !sw.empty() ? "    " : "",
                 pages > 1 && !sw.empty() ? (sw + ": page").c_str() : "");
    centred(dl, font, unit * 0.78f, ImVec2(mid.x, mid.y + l.hintY), scaled(IM_COL32(210, 210, 215, 220), fade), hint);
    // The pointer, as on the wheel.
    dl->AddCircleFilled(ImVec2(mid.x + pointer.x, mid.y + pointer.y), unit * 0.3f, scaled(IM_COL32(255, 255, 255, 240), fade), 16);
    dl->AddCircle(ImVec2(mid.x + pointer.x, mid.y + pointer.y), unit * 0.3f, scaled(IM_COL32(0, 0, 0, 220), fade), 16, 1.5f);
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
    if (g_mode == Mode::Players) { // 1-9: go to that player of the page
        if (g_state != State::Open || n < 1 || n > kPerPage) return;
        const size_t at = static_cast<size_t>(g_page) * kPerPage + static_cast<size_t>(n - 1);
        if (at >= g_players.size()) return;
        actOn(n - 1, Act::GoTo);
        setState(State::Used);
        return;
    }
    if ((g_state != State::Held && g_state != State::Open) || n < 1 || n > slotCount()) return;
    place(n - 1);
    setState(State::Used);
}

void moved(float dx, float dy, float cursorScale) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state != State::Open) return;
    if (g_mode == Mode::Players) { // a cursor like the menu's, kept inside the panel
        g_x = std::clamp(g_x + dx * cursorScale, -g_layout.halfW, g_layout.halfW);
        g_y = std::clamp(g_y + dy * cursorScale, -g_layout.halfH, g_layout.halfH);
        return;
    }
    const float sens = std::clamp(config::get()->wheelSensitivity, 0.1f, 3.f) * cursorScale;
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

namespace {
// The panel's click and right click (g_m held, the panel open).
// Ticks or unticks page slot `i` for "Summon picked" (the panel stays open).
void togglePick(int i) {
    const size_t at = static_cast<size_t>(g_page) * kPerPage + static_cast<size_t>(i);
    if (i < 0 || at >= g_players.size()) return;
    const uint32_t id = g_players[at].id;
    const auto it = std::find(g_picked.begin(), g_picked.end(), id);
    if (it != g_picked.end()) g_picked.erase(it);
    else g_picked.push_back(id);
    g_picksGone = false;
}

void panelClick() {
    const int i = pointedPlayer();
    if (i >= 0) {
        togglePick(i); // the panel stays open
    } else if (g_layout.host && g_layout.hardBox.has(g_x, g_y)) {
        g_hard = !g_hard; // the panel stays open
    } else if (g_layout.allBox.has(g_x, g_y)) {
        if (g_picksGone && !g_withdraw) { // the players picked left: say so; the next press summons everyone
            g_picksGone = false;
            g_note = "The picked players left";
            return;
        }
        Action a;
        if (g_withdraw) {
            a.act = Act::Withdraw;
        } else {
            a.act = Act::SummonGroup;
            a.targets = g_picked; // empty = everyone
            for (const Player& p : g_players) // one picked player: the notice names them
                if (g_picked.size() == 1 && p.id == g_picked.front()) a.name = p.name;
            a.hard = g_hard && g_layout.host;
            for (const Player& p : g_players) // a picked player in a run hears of it when the run is over
                if (p.inRun && (g_picked.empty() || picked(p.id))) a.inRun = true;
        }
        g_action = a;
        setState(State::Used);
    }
}
void panelCall() { // right click: go to the player pointed at (elsewhere: just close)
    const int i = pointedPlayer();
    if (i >= 0) actOn(i, Act::GoTo);
    setState(State::Used);
}
} // namespace

void click() {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state != State::Open) return;
    if (g_mode == Mode::Players) {
        panelClick();
        return;
    }
    const int slot = pointedSlot();
    if (slot >= 0) place(slot);
    setState(State::Used);
}

void dismiss() {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state != State::Open) return;
    if (g_mode == Mode::Players) panelCall();
    else setState(State::Used); // closed; the release then places nothing
}

void switchWheel(int step) {
    std::lock_guard<std::mutex> lock(g_m);
    stepWheel(step);
}

void cancel() {
    std::lock_guard<std::mutex> lock(g_m);
    setState(State::Idle);
}

bool padUpdate(Mode mode, bool pressed, bool held, float x, float y, bool prev, bool next, bool clear, bool go, bool call) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_state == State::Idle) {
        if (!pressed || !held) return false;
        begin(Source::Pad, 0, mode);
        if (g_state == State::Idle) return false; // nothing to answer
    }
    if (g_source != Source::Pad) return false; // the keyboard has it
    const DWORD now = GetTickCount();
    const float dt = std::min(static_cast<float>(now - g_padAt), 50.f) / 1000.f; // since the last read, in seconds
    g_padAt = now;
    if (!held) {
        if (g_state == State::Used) setState(State::Idle);
        else release();
        return false;
    }
    promote(now);
    if (clear) clearNow();
    if (g_state != State::Open) return false;
    if (g_mode == Mode::Players) {
        // The stick moves the pointer like a mouse would (it stays where it is let go); a light push moves it slowly.
        auto axis = [](float v) {
            v = std::clamp(v, -1.f, 1.f);
            const float a = std::max(0.f, (std::fabs(v) - kPadPanelDead) / (1.f - kPadPanelDead));
            return std::copysign(a * a, v);
        };
        const float speed = g_unit * kPadPanelSpeed * dt;
        g_x = std::clamp(g_x + axis(x) * speed, -g_layout.halfW, g_layout.halfW);
        g_y = std::clamp(g_y + axis(y) * speed, -g_layout.halfH, g_layout.halfH);
        if (prev || next) stepWheel(1); // LB (RB is usually part of the panel's buttons): the next page, round
        if (go) panelClick();
        else if (call) panelCall();
        return true;
    }
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
    const float unit = ImGui::GetFontSize() * kWheelScale;
    std::vector<Player> players;
    if (g_listing.load()) players = readPlayers();
    Action act;
    Layout layout;
    std::vector<Player> page;
    int pageNo = 0, pages = 1;
    bool hard = false, withdraw = false;
    std::vector<uint32_t> pickedIds;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_note.empty()) {
            notices::push(g_note);
            g_note.clear();
        }
        act = g_action;
        g_action = Action{};
        if (g_listing.load()) {
            g_players = std::move(players);
            g_page = std::min(g_page, pageCount() - 1);
            const size_t hadPicks = g_picked.size();
            g_picked.erase(std::remove_if(g_picked.begin(), g_picked.end(),
                                          [](uint32_t id) {
                                              return std::none_of(g_players.begin(), g_players.end(),
                                                                  [id](const Player& p) { return p.id == id; });
                                          }),
                           g_picked.end()); // a player who left is no longer picked
            // Every picked player left: the button must not quietly become "Summon everyone" under the pointer.
            if (hadPicks && g_picked.empty()) g_picksGone = true;
        }
    }
    perform(act);
    tickWait();
    drawWait(ImGui::GetForegroundDrawList(), ImGui::GetFont(), unit);
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
        if (g_mode == Mode::Players) {
            const size_t first = static_cast<size_t>(g_page) * kPerPage;
            for (size_t i = first; i < g_players.size() && i < first + kPerPage; ++i) page.push_back(g_players[i]);
            g_layout = layoutPanel(static_cast<int>(page.size()), net::canHardSummon(), unit);
            g_x = std::clamp(g_x, -g_layout.halfW, g_layout.halfW);
            g_y = std::clamp(g_y, -g_layout.halfH, g_layout.halfH);
            layout = g_layout;
            pageNo = g_page;
            pages = pageCount();
            hard = g_hard;
            g_withdraw = net::summonOpen();
            withdraw = g_withdraw;
            pickedIds = g_picked;
            source = g_source;
            hot = pointedPlayer();
            ox = g_x;
            oy = g_y;
            if (g_instant) fade = std::clamp(static_cast<float>(now - g_downAt) / kFadeInMs, 0.f, 1.f);
        }
        mode = g_mode;
    }
    if (mode == Mode::Players) {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        drawPanel(ImGui::GetForegroundDrawList(), ImGui::GetFont(), unit, ImVec2(screen.x * 0.5f, screen.y * 0.5f), layout, page,
                  pageNo, pages, hot, hard, ImVec2(ox, oy), fade, source == Source::Pad, pickedIds, withdraw);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_state != State::Open) return;
        const ImVec2 shown = shownPointer(unit);
        ox = shown.x;
        oy = shown.y;
        wheel = g_wheel;
        source = g_source;
        mode = g_mode;
        n = slotCount();
        hot = pointedSlot();
        if (g_instant) fade = std::clamp(static_cast<float>(GetTickCount() - g_downAt) / kFadeInMs, 0.f, 1.f);
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
        uint8_t rgb[3], accent[3]; // each marker in its own colors
        config::markerColors(*cfg, slots[i], rgb, accent);
        const int segs = std::max(6, static_cast<int>(14.f * step / 0.70f));
        dl->PathArcTo(mid, rOut, a0, a1, segs);
        dl->PathArcTo(mid, rIn, a1, a0, segs);
        // A slot without a title is not set: drawn at half opacity (it still works, as an icon only).
        const bool unset = slots[i].label.empty();
        dl->PathFillConcave(scaled(on ? rgba(rgb, unset ? 100 : 175) : IM_COL32(24, 24, 30, unset ? 110 : 205), fade));
        dl->PathArcTo(mid, rOut, a0, a1, segs);
        dl->PathArcTo(mid, rIn, a1, a0, segs);
        dl->PathStroke(scaled(on ? IM_COL32(255, 255, 255, unset ? 140 : 235) : IM_COL32(255, 255, 255, unset ? 30 : 60), fade),
                       ImDrawFlags_Closed, on ? 2.5f : 1.f);

        const float dx = std::cos(centre), dy = std::sin(centre);
        // The slot's digit at the inner ring, its icon further out.
        snprintf(num, sizeof(num), "%d", i + 1);
        centred(dl, font, unit * 0.8f, ImVec2(mid.x + dx * (rIn + unit * 0.65f), mid.y + dy * (rIn + unit * 0.65f)),
                scaled(on ? IM_COL32(255, 255, 255, unset ? 140 : 255) : IM_COL32(200, 200, 205, unset ? 110 : 220), fade), num);
        const float rIcon = rIn + (rOut - rIn) * 0.6f;
        const int iconA = unset ? 128 : 255;
        drawGlyph(dl, slots[i].glyph, ImVec2(mid.x + dx * rIcon, mid.y + dy * rIcon), unit * (on ? 1.15f : 0.95f),
                  on ? IM_COL32(255, 255, 255, iconA) : rgba(rgb, iconA), fade * (unset ? 0.5f : 1.f), rgba(accent, iconA));
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
