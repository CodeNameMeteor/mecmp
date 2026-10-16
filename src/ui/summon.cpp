#include "core/log.h"
#include "ui/summon.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/config.h"
#include "game/camera.h"
#include "game/camera_math.h"
#include "game/hooks.h"
#include "game/teleport.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "net/summon_policy.h"
#include "ui/gamepad.h"
#include "ui/glyph.h"
#include "ui/notices.h"
#include "ui/overlay.h"

namespace mecmp::ui::summon {
namespace {
namespace sp = net::summon;

constexpr DWORD kHoldMs = 1000;        // accept, decline, send hard, withdraw: hold this long
constexpr DWORD kReturnHoldMs = 3000;  // going back needs a longer hold: it is easy to do by mistake
constexpr size_t kMaxPending = 4;
constexpr float kBeaconFullUntil = 150.f; // a summon point is a destination: it fades only far away
constexpr float kBeaconGoneAt = 600.f;
constexpr float kNearEnoughM = 5.f;       // closer than this to the point: the summon has done its job

struct Pending {
    uint32_t id = 0;
    uint32_t from = 0;
    std::string name;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    bool hard = false;
    bool visit = false; // not a summon: `from` asks to come to us (VISIT); no position, no beacon
    DWORD at = 0;
};
std::vector<Pending> g_pending; // render thread only; the one shown in the panel is the oldest
std::atomic<bool> g_takeAccept{false}, g_takeDecline{false};

// Hold tracking. A key already down when it starts to matter does not count until it was released once.
struct Hold {
    DWORD since = 0;
    bool armed = false;
    // Returns how far along the hold is (0..1); `done` once it got there.
    float step(bool down, DWORD now, DWORD need, bool& done) {
        done = false;
        if (!down) {
            since = 0;
            armed = true;
            return 0.f;
        }
        if (!armed) return 0.f;
        if (!since) since = now | 1;
        const DWORD held = now - since;
        if (held >= need) {
            done = true;
            since = 0;
            armed = false; // one action per press
            return 1.f;
        }
        return static_cast<float>(held) / static_cast<float>(need);
    }
    void reset() { since = 0; armed = false; }
};
Hold g_accept, g_decline, g_return, g_dismiss; // g_dismiss: the accept key hiding the "Go back?" bar
float g_acceptProgress = 0.f, g_declineProgress = 0.f, g_returnProgress = 0.f, g_dismissProgress = 0.f;
uint32_t g_topId = 0; // which pending summon the holds belong to

std::string mmss(uint32_t ms) {
    char b[16];
    const uint32_t s = (ms + 999) / 1000;
    snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60);
    return b;
}

std::string keyHint(const std::string& key, const std::string& pad) {
    std::string s = key.empty() ? std::string() : keyLabel(key);
    if (gamepad::active() && !pad.empty()) s += (s.empty() ? "" : " / ") + gamepad::labelCombo(pad);
    return s.empty() ? std::string("?") : s;
}

void removePending(uint32_t id, uint32_t from) {
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                   [&](const Pending& p) { return p.id == id && p.from == from; }),
                    g_pending.end());
}

void go(const Pending& p) {
    if (p.visit) { // yes: they come to us
        net::sendVisit(p.from, net::protocol::kVisitYes, p.id);
        notices::push(p.name + " is coming to you");
        removePending(p.id, p.from);
        return;
    }
    const std::string name = p.name;
    const bool ok = game::teleport::requestToPos(p.pos, p.yaw);
    net::sendSummonReply(p.id, p.from, ok ? net::protocol::kSummonTeleporting : net::protocol::kSummonBusy);
    if (!ok) MLOG("summon: could not go to %s: the teleport is busy or not available", name.c_str());
    notices::push(ok ? "Going to " + name : "Can't go right now");
    removePending(p.id, p.from);
}

void decline(const Pending& p) {
    if (p.visit) net::sendVisit(p.from, net::protocol::kVisitNo, p.id);
    else net::sendSummonReply(p.id, p.from, net::protocol::kSummonDeclined);
    removePending(p.id, p.from);
}

// The local player's position, for distances. False when not in a level.
bool here(float out[3]) {
    static net::PlayerState st; // large; render thread only
    if (!net::localState(st)) return false;
    for (int i = 0; i < 3; ++i) out[i] = st.pos[i];
    return true;
}

void textShadow(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const char* text) {
    dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(font, size, at, col, text);
}

// One "hold this key" button: a rounded box with the key and what it does, filled by `progress`.
// Returns its width.
float holdButton(ImDrawList* dl, ImFont* font, float size, ImVec2 at, const std::string& key, const char* what,
                 float progress, ImU32 tint) {
    char text[96];
    snprintf(text, sizeof(text), "[%s]  %s", key.c_str(), what);
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    const float pad = size * 0.5f;
    const ImVec2 a = at, b(at.x + ts.x + 2 * pad, at.y + ts.y + pad);
    dl->AddRectFilled(a, b, IM_COL32(20, 20, 28, 220), size * 0.3f);
    if (progress > 0.f)
        dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::clamp(progress, 0.f, 1.f), b.y), tint, size * 0.3f);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 120), size * 0.3f);
    dl->AddText(font, size, ImVec2(a.x + pad, a.y + pad * 0.5f), IM_COL32(255, 255, 255, 255), text);
    return b.x - a.x;
}

// A box centred at the top with a title line and a row of buttons below it.
struct Panel {
    ImDrawList* dl;
    ImFont* font;
    float size;
    ImVec2 screen;
};

void drawPending(const Panel& pn, const Pending& p, DWORD now, float distance, const std::string& acceptKey,
                 const std::string& declineKey) {
    const auto cfg = config::get();
    char title[160];
    if (p.visit) {
        snprintf(title, sizeof(title), "%s wants to come to you", p.name.c_str());
    } else if (p.hard) {
        const DWORD grace = static_cast<DWORD>(cfg->hardSummonGraceMs);
        const DWORD since = now - p.at;
        const DWORD left = since >= grace ? 0 : grace - since;
        snprintf(title, sizeof(title), "%s is pulling you in: %u", p.name.c_str(), static_cast<unsigned>((left + 999) / 1000));
    } else {
        snprintf(title, sizeof(title), "%s summons you  (%.0f m)", p.name.c_str(), distance);
    }
    const float size = pn.size;
    const ImVec2 ts = pn.font->CalcTextSizeA(size, FLT_MAX, 0.f, title);
    const float pad = size * 0.55f;
    const float y = pn.screen.y * 0.06f + size * 9.f; // below the notices
    const ImVec2 a(pn.screen.x * 0.5f - ts.x * 0.5f - pad, y);
    const ImVec2 b(a.x + ts.x + 2 * pad, y + ts.y + pad);
    const ImU32 tint = p.hard ? IM_COL32(255, 110, 70, 255) : IM_COL32(80, 220, 140, 255);
    pn.dl->AddRectFilled(a, b, IM_COL32(12, 12, 18, 215), size * 0.3f);
    pn.dl->AddRect(a, b, tint, size * 0.3f, 0, 2.f);
    pn.dl->AddText(pn.font, size, ImVec2(a.x + pad, a.y + pad * 0.5f), IM_COL32(255, 255, 255, 255), title);
    // The time left, as a thin line along the bottom of the title box.
    const DWORD window = p.hard ? static_cast<DWORD>(cfg->hardSummonGraceMs) : sp::kSoftWindowMs;
    if (window) {
        const float f = 1.f - std::clamp(static_cast<float>(now - p.at) / static_cast<float>(window), 0.f, 1.f);
        pn.dl->AddRectFilled(ImVec2(a.x + 4, b.y - 4), ImVec2(a.x + 4 + (b.x - a.x - 8) * f, b.y - 1), tint);
    }
    // The two buttons side by side under it.
    const char* yes = p.visit ? "Yes" : p.hard ? "Go now" : "Go";
    const char* no = p.hard ? "Cancel" : "No";
    ImFont* f = pn.font;
    const ImVec2 t1 = f->CalcTextSizeA(size, FLT_MAX, 0.f, (std::string("[") + acceptKey + "]  " + yes).c_str());
    const ImVec2 t2 = f->CalcTextSizeA(size, FLT_MAX, 0.f, (std::string("[") + declineKey + "]  " + no).c_str());
    const float gap = size * 0.6f, bw = size;
    const float total = t1.x + t2.x + 4 * bw + gap;
    float x = pn.screen.x * 0.5f - total * 0.5f;
    const float by = b.y + size * 0.35f;
    x += holdButton(pn.dl, f, size, ImVec2(x, by), acceptKey, yes, g_acceptProgress, tint) + gap;
    holdButton(pn.dl, f, size, ImVec2(x, by), declineKey, no, g_declineProgress, IM_COL32(200, 200, 210, 255));
}

// "Go back?" for a minute after a summon took us: holding the decline key goes back, holding the accept
// key hides the bar (and gives up going back). The bar fills for whichever is held.
void drawReturn(const Panel& pn, uint32_t leftMs, const std::string& acceptKey, const std::string& declineKey) {
    char text[128];
    snprintf(text, sizeof(text), "Go back?  Hold [%s]  %s    Hold [%s] to hide", declineKey.c_str(), mmss(leftMs).c_str(),
             acceptKey.c_str());
    const float size = pn.size;
    const ImVec2 ts = pn.font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    const float pad = size * 0.55f;
    const float y = pn.screen.y * 0.06f + size * 9.f;
    const ImVec2 a(pn.screen.x * 0.5f - ts.x * 0.5f - pad, y);
    const ImVec2 b(a.x + ts.x + 2 * pad, y + ts.y + pad);
    pn.dl->AddRectFilled(a, b, IM_COL32(12, 12, 18, 215), size * 0.3f);
    if (g_returnProgress > 0.f)
        pn.dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::clamp(g_returnProgress, 0.f, 1.f), b.y),
                             IM_COL32(120, 170, 255, 200), size * 0.3f);
    else if (g_dismissProgress > 0.f)
        pn.dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::clamp(g_dismissProgress, 0.f, 1.f), b.y),
                             IM_COL32(150, 150, 160, 200), size * 0.3f);
    pn.dl->AddRect(a, b, IM_COL32(120, 170, 255, 255), size * 0.3f, 0, 2.f);
    pn.dl->AddText(pn.font, size, ImVec2(a.x + pad, a.y + pad * 0.5f), IM_COL32(255, 255, 255, 255), text);
}

// The summon point in the world: a ring with a chevron over it, "Summon point" and the caller's name.
void drawBeacons(const Panel& pn) {
    game::CameraView cam;
    if (g_pending.empty() || !game::camera::read(cam)) return;
    const float unit = pn.size;
    const float margin = unit * 2.2f;
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(GetTickCount() % 100000) * 0.006f);
    for (const Pending& p : g_pending) {
        if (p.visit) continue; // a visitor comes here: no point to show
        const float at[3] = {p.pos[0], p.pos[1] + net::protocol::kPingLift, p.pos[2]};
        game::EdgeIndicator e;
        if (!game::edgeIndicator(cam, at, pn.screen.x, pn.screen.y, margin, e)) continue;
        const float alpha = std::max(0.35f, game::distanceFade(e.distance, kBeaconFullUntil, kBeaconGoneAt));
        const uint8_t rgb[3] = {static_cast<uint8_t>(p.hard ? 255 : 80), static_cast<uint8_t>(p.hard ? 110 : 220),
                                static_cast<uint8_t>(p.hard ? 70 : 140)};
        const ImU32 fill = IM_COL32(rgb[0], rgb[1], rgb[2], static_cast<int>(255 * alpha));
        char text[96];
        snprintf(text, sizeof(text), "%s  %.0f m", p.name.c_str(), e.distance);
        if (e.onScreen) {
            const float r = unit * std::clamp(1.2f - e.distance / 160.f, 0.7f, 1.2f);
            ImDrawList* dl = pn.dl;
            dl->AddCircle(ImVec2(e.x, e.y), r * (1.3f + 0.5f * pulse), fill, 0, unit * 0.18f);
            drawGlyph(dl, 7, ImVec2(e.x, e.y), r, fill, alpha);       // a ring: this very spot
            drawGlyph(dl, 1, ImVec2(e.x, e.y - r * 1.9f), r * 0.8f, fill, alpha); // a chevron over it
            const ImVec2 t1 = pn.font->CalcTextSizeA(unit * 1.05f, FLT_MAX, 0.f, "Summon point");
            const ImVec2 t2 = pn.font->CalcTextSizeA(unit * 0.85f, FLT_MAX, 0.f, text);
            const float top = e.y - r * 2.9f - t1.y - t2.y;
            textShadow(dl, pn.font, unit * 1.05f, ImVec2(e.x - t1.x * 0.5f, top), IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)), "Summon point");
            textShadow(dl, pn.font, unit * 0.85f, ImVec2(e.x - t2.x * 0.5f, top + t1.y), IM_COL32(255, 255, 255, static_cast<int>(230 * alpha)), text);
        } else {
            const float size = unit * 0.8f;
            const ImVec2 tip(e.x + e.dirX * size, e.y + e.dirY * size);
            const ImVec2 back(e.x - e.dirX * size * 0.6f, e.y - e.dirY * size * 0.6f);
            const ImVec2 side(-e.dirY * size * 0.7f, e.dirX * size * 0.7f);
            const ImVec2 a(back.x + side.x, back.y + side.y), b(back.x - side.x, back.y - side.y);
            pn.dl->AddTriangleFilled(tip, a, b, fill);
            pn.dl->AddTriangle(tip, a, b, IM_COL32(0, 0, 0, 200), 1.5f);
            const ImVec2 ts = pn.font->CalcTextSizeA(unit * 0.85f, FLT_MAX, 0.f, text);
            const ImVec2 where(std::clamp(e.x - e.dirX * size * 2.6f - ts.x * 0.5f, 4.f, std::max(4.f, pn.screen.x - ts.x - 4.f)),
                               std::clamp(e.y - e.dirY * size * 2.2f - ts.y * 0.5f, 4.f, std::max(4.f, pn.screen.y - ts.y - 4.f)));
            textShadow(pn.dl, pn.font, unit * 0.85f, where, IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)), text);
            textShadow(pn.dl, pn.font, unit * 0.85f, ImVec2(where.x, where.y - unit * 0.9f), IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)), "Summon point");
        }
    }
}
} // namespace

bool takesAcceptKey() { return g_takeAccept.load(); }
bool takesDeclineKey() { return g_takeDecline.load(); }

bool send(const std::vector<uint32_t>& targets, bool hard, const std::string& name) {
    static net::PlayerState st; // large; render thread only
    if (net::inRun()) { // they would come to you mid-run
        notices::push("You are in a run: finish or quit it first");
        return false;
    }
    if (!net::localState(st)) {
        notices::push("Can't summon anyone from here");
        return false;
    }
    const bool canHard = net::canHardSummon();
    const bool asHard = hard && canHard;
    if (!net::sendSummon(st.pos, st.yaw, targets, asHard)) {
        const uint32_t left = net::summonCooldownLeftMs();
        notices::push(left ? "Wait " + std::to_string((left + 999) / 1000) + " s before summoning again"
                           : std::string("Nobody to summon"));
        return false;
    }
    const std::string who = targets.empty() ? std::string("everyone")
                            : targets.size() == 1 ? (name.empty() ? std::string("1 player") : name)
                                                  : std::to_string(targets.size()) + " players";
    notices::push(asHard ? "Pulling " + who + " to you" : "Summoning " + who + " to you");
    if (hard && !canHard)
        notices::push("Only the host (or a member it allows) can pull players in: sent as a plain summon");
    return true;
}

// Our own Go to on a player who asks first (VISIT): one at a time. target 0 = none.
namespace {
struct Asking {
    uint32_t target = 0, id = 0;
    std::string name;
    DWORD at = 0;
};
Asking g_asking;
// No answer in this long: given up. A little longer than the other side's window (sp::kSoftWindowMs), so a
// yes given at its very end still arrives while we wait.
constexpr DWORD kAskWindowMs = sp::kSoftWindowMs + 5000;

// VISIT packets addressed to us: someone asks to come (or gave up asking), or answers our own ask.
void takeVisits(DWORD now) {
    for (const net::VisitInfo& v : net::takeVisits()) {
        switch (v.kind) {
        case net::protocol::kVisitAsk: {
            // Switched off since our HELLO said so: just say yes (not in a run: then it waits like any ask).
            if (!config::get()->askBeforeVisit && !net::inRun()) {
                net::sendVisit(v.from, net::protocol::kVisitYes, v.id);
                break;
            }
            g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                           [&](const Pending& p) { return p.from == v.from && p.visit; }),
                            g_pending.end()); // a new ask from the same player replaces the old one
            Pending p;
            p.id = v.id;
            p.from = v.from;
            p.name = v.name;
            p.visit = true;
            p.at = now;
            g_pending.push_back(p);
            while (g_pending.size() > kMaxPending) g_pending.erase(g_pending.begin());
            notices::push(net::inRun() ? v.name + " wants to come to you: you can answer when your run is over"
                                       : v.name + " wants to come to you");
            break;
        }
        case net::protocol::kVisitWithdraw: removePending(v.id, v.from); break;
        case net::protocol::kVisitYes:
        case net::protocol::kVisitNo: {
            if (!g_asking.target || v.from != g_asking.target || v.id != g_asking.id) break; // not (or no longer) ours
            const Asking a = g_asking;
            g_asking = Asking{};
            if (v.kind == net::protocol::kVisitNo) {
                notices::push(a.name + " said no");
            } else if (net::inRun()) {
                notices::push(a.name + " said yes, but you are in a run");
            } else if (!game::teleport::available() || game::teleport::busy()) {
                notices::push(a.name + " said yes, but you can't go right now");
            } else {
                game::teleport::requestGoTo(a.target);
                notices::push("Going to " + a.name);
            }
            break;
        }
        default: break;
        }
    }
    if (g_asking.target && now - g_asking.at >= kAskWindowMs) {
        net::sendVisit(g_asking.target, net::protocol::kVisitWithdraw, g_asking.id);
        notices::push(g_asking.name + " did not answer");
        g_asking = Asking{};
    }
}
} // namespace

bool askVisit(uint32_t target, const std::string& name) {
    if (net::inRun() && g_asking.target != target) {
        notices::push("You are in a run: finish or quit it first");
        return false;
    }
    if (g_asking.target == target) { // the same player again: give up asking
        net::sendVisit(target, net::protocol::kVisitWithdraw, g_asking.id);
        notices::push("Stopped asking " + name);
        g_asking = Asking{};
        return false;
    }
    if (g_asking.target) net::sendVisit(g_asking.target, net::protocol::kVisitWithdraw, g_asking.id); // one at a time
    const uint32_t id = net::sendVisit(target, net::protocol::kVisitAsk);
    if (!id) {
        notices::push("Not in a session");
        g_asking = Asking{};
        return false;
    }
    g_asking = {target, id, name, GetTickCount()};
    notices::push("Asked " + name + " if you can come");
    return true;
}

static std::atomic<bool> g_levelChanged{false}; // set by levelChanged() on any thread

void levelChanged() { g_levelChanged = true; }

void update(const Input& in) {
    const DWORD now = GetTickCount();
    if (g_levelChanged.exchange(false)) {
        g_pending.clear();
        if (g_asking.target) net::sendVisit(g_asking.target, net::protocol::kVisitWithdraw, g_asking.id);
        g_asking = Asking{}; // a yes for the old level must not move us
        g_takeAccept = false;
        g_takeDecline = false;
    }

    // What other players sent: new summons, withdrawals.
    for (const net::SummonInfo& s : net::takeSummons()) {
        VLOG("summon: %s from %08x", s.cancel ? "withdrawn" : "received", s.from);
        if (s.cancel) {
            removePending(s.id, s.from);
            continue;
        }
        g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                       [&](const Pending& p) { return p.from == s.from && !p.visit; }),
                        g_pending.end()); // a new call from the same player replaces its old one
        Pending p;
        p.id = s.id;
        p.from = s.from;
        p.name = s.name;
        memcpy(p.pos, s.pos, sizeof(p.pos));
        p.yaw = s.yaw;
        p.hard = s.hard;
        p.at = now;
        g_pending.push_back(p);
        while (g_pending.size() > kMaxPending) g_pending.erase(g_pending.begin());
        if (net::inRun()) notices::push(s.name + " summons you: you can answer when your run is over");
        else if (!s.hard) notices::push(s.name + " summons you");
    }
    takeVisits(now);
    // In a run (a time trial, a dash): summons wait, unseen, and their time (a hard one's countdown too)
    // starts only once the run is over, finished or quit.
    const bool running = net::inRun();
    if (running)
        for (Pending& p : g_pending) p.at = now;
    // What happened to ours.
    for (const std::string& line : net::takeSummonReports()) notices::push(line);

    // Out of time (soft: they stop being offered a minute after they came), or already there.
    float me[3];
    const bool haveMe = here(me);
    for (size_t i = 0; i < g_pending.size();) {
        const Pending& p = g_pending[i];
        bool drop = !p.hard && now - p.at >= sp::kSoftWindowMs;
        if (haveMe && !p.visit) {
            float d2 = 0.f;
            for (int k = 0; k < 3; ++k) d2 += (p.pos[k] - me[k]) * (p.pos[k] - me[k]);
            if (d2 < kNearEnoughM * kNearEnoughM) {
                net::sendSummonReply(p.id, p.from, net::protocol::kSummonTeleporting); // already here: as good as there
                drop = true;
            }
        }
        if (drop) g_pending.erase(g_pending.begin() + static_cast<long>(i));
        else ++i;
    }

    // The panel's keys: hold one for a second.
    const bool pendingShown = !g_pending.empty() && !running;
    const uint32_t topId = pendingShown ? g_pending.front().id : 0;
    if (topId != g_topId) { // a different summon is on top: keys held from before do not count
        g_topId = topId;
        g_accept.reset();
        g_decline.reset();
    }
    const uint32_t returnLeft = running ? 0 : game::teleport::returnLeftMs(); // no "Go back?" in a run
    g_takeAccept = pendingShown || returnLeft > 0;
    g_takeDecline = pendingShown || returnLeft > 0;
    g_acceptProgress = g_declineProgress = g_returnProgress = g_dismissProgress = 0.f;
    if (!in.canAct) {
        g_accept.reset();
        g_decline.reset();
        g_return.reset();
        g_dismiss.reset();
        return;
    }
    bool done = false;
    if (pendingShown) {
        const Pending top = g_pending.front();
        g_acceptProgress = g_accept.step(in.acceptDown, now, kHoldMs, done);
        if (done) {
            go(top);
            return;
        }
        g_declineProgress = g_decline.step(in.declineDown, now, kHoldMs, done);
        if (done) {
            decline(top);
            return;
        }
        // A hard summon that ran out of time takes the player now (not while a menu is up).
        if (top.hard && now - top.at >= static_cast<DWORD>(config::get()->hardSummonGraceMs) &&
            !game::hooks::gameMenuOpen() && !menuOpen()) {
            go(top);
            return;
        }
        g_return.reset();
        g_dismiss.reset();
    } else if (returnLeft > 0) {
        g_returnProgress = g_return.step(in.declineDown, now, kReturnHoldMs, done);
        if (done) {
            if (game::teleport::requestReturn()) notices::push("Going back");
            return;
        }
        g_dismissProgress = g_dismiss.step(in.acceptDown, now, kHoldMs, done);
        if (done) game::teleport::forgetReturn(); // the bar goes, and going back with it
    } else {
        g_accept.reset();
        g_decline.reset();
        g_return.reset();
        g_dismiss.reset();
    }
}

void draw() {
    const auto cfg = config::get();
    const ImGuiIO& io = ImGui::GetIO();
    Panel pn{ImGui::GetForegroundDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), io.DisplaySize};
    const DWORD now = GetTickCount();
    if (g_asking.target && !overlayHidden()) {
        const std::string line = "Waiting for " + g_asking.name + " to answer";
        const ImVec2 ts = pn.font->CalcTextSizeA(pn.size * 0.85f, FLT_MAX, 0.f, line.c_str());
        textShadow(pn.dl, pn.font, pn.size * 0.85f, ImVec2(pn.screen.x * 0.5f - ts.x * 0.5f, pn.screen.y * 0.06f + pn.size * 1.2f),
                   IM_COL32(255, 255, 255, 200), line.c_str());
    }
    if (net::inRun()) { // summons wait for the end of the run (update)
        if (!g_pending.empty() && !overlayHidden()) {
            const std::string line = g_pending.size() == 1 ? g_pending.front().name + " summoned you: answer after your run"
                                                           : std::to_string(g_pending.size()) + " summons wait for the end of your run";
            const ImVec2 ts = pn.font->CalcTextSizeA(pn.size * 0.85f, FLT_MAX, 0.f, line.c_str());
            textShadow(pn.dl, pn.font, pn.size * 0.85f, ImVec2(pn.screen.x * 0.5f - ts.x * 0.5f, pn.screen.y * 0.06f),
                       IM_COL32(255, 255, 255, 200), line.c_str());
        }
    } else if (!overlayHidden()) {
        drawBeacons(pn);
    }
    // The panel is a warning (a hard summon is about to move the player), so it is not hidden with the overlay.
    if (!g_pending.empty() && !net::inRun()) {
        float me[3] = {0, 0, 0};
        const bool haveMe = here(me);
        const Pending& p = g_pending.front();
        float d = 0.f;
        if (haveMe) {
            for (int k = 0; k < 3; ++k) d += (p.pos[k] - me[k]) * (p.pos[k] - me[k]);
            d = std::sqrt(d);
        }
        drawPending(pn, p, now, d, keyHint(cfg->summonAcceptKey, cfg->summonAcceptPad),
                    keyHint(cfg->summonDeclineKey, cfg->summonDeclinePad));
        return;
    }
    if (const uint32_t left = game::teleport::returnLeftMs(); left > 0 && !game::teleport::busy() && !net::inRun())
        drawReturn(pn, left, keyHint(cfg->summonAcceptKey, cfg->summonAcceptPad),
                   keyHint(cfg->summonDeclineKey, cfg->summonDeclinePad));
}

} // namespace mecmp::ui::summon
