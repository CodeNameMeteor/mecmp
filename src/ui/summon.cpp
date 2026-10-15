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
Hold g_accept, g_decline, g_return;
float g_acceptProgress = 0.f, g_declineProgress = 0.f, g_returnProgress = 0.f;
uint32_t g_topId = 0; // which pending summon the holds belong to

// The send key: a tap calls everyone, a one-second hold pulls them in, or withdraws an open call.
DWORD g_sendSince = 0;
bool g_sendFired = false, g_sendWithdraws = false;
float g_sendProgress = 0.f;
// The hold's bar: shown after kHintDelayMs of holding, faded in and out over kHintFadeMs.
constexpr DWORD kHintDelayMs = 250;
constexpr DWORD kHintFadeMs = 100;
bool g_hintTarget = false;    // the bar should be shown now
bool g_hintWithdraw = false;  // what it says (kept while it fades out)
bool g_hintCanHard = false;   // ...and whether holding on will really pull players in
float g_hintProgress = 0.f;   // how full it is (kept while it fades out)
float g_hintAlpha = 0.f;      // 0..1, as drawn

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
    const std::string name = p.name;
    const bool ok = game::teleport::requestToPos(p.pos, p.yaw);
    net::sendSummonReply(p.id, p.from, ok ? net::protocol::kSummonTeleporting : net::protocol::kSummonBusy);
    notices::push(ok ? "Going to " + name : "Can't go right now");
    removePending(p.id, p.from);
}

void decline(const Pending& p) {
    net::sendSummonReply(p.id, p.from, net::protocol::kSummonDeclined);
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
    if (p.hard) {
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
    const char* yes = p.hard ? "Go now" : "Go";
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

void drawReturn(const Panel& pn, uint32_t leftMs, const std::string& declineKey) {
    char text[96];
    snprintf(text, sizeof(text), "Go back?  Hold [%s]  %s", declineKey.c_str(), mmss(leftMs).c_str());
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

bool send(const std::vector<uint32_t>& targets, bool hard) {
    static net::PlayerState st; // large; render thread only
    if (!net::localState(st)) {
        notices::push("Can't call anyone from here");
        return false;
    }
    const bool canHard = net::canHardSummon();
    const bool asHard = hard && canHard;
    if (!net::sendSummon(st.pos, st.yaw, targets, asHard)) {
        const uint32_t left = net::summonCooldownLeftMs();
        notices::push(left ? "Wait " + std::to_string((left + 999) / 1000) + " s before calling again"
                           : std::string("Nobody to call"));
        return false;
    }
    const std::string who = targets.empty() ? std::string("everyone")
                            : targets.size() == 1 ? std::string("1 player")
                                                  : std::to_string(targets.size()) + " players";
    notices::push(asHard ? "Pulling " + who + " to you" : "Calling " + who + " to you");
    if (hard && !canHard)
        notices::push("Only the host (or a member it allows) can pull players in: called as a plain summon");
    return true;
}

static std::atomic<bool> g_levelChanged{false}; // set by levelChanged() on any thread

void levelChanged() { g_levelChanged = true; }

void update(const Input& in) {
    const DWORD now = GetTickCount();
    if (g_levelChanged.exchange(false)) {
        g_pending.clear();
        g_takeAccept = false;
        g_takeDecline = false;
    }

    // What other players sent: new summons, withdrawals.
    for (const net::SummonInfo& s : net::takeSummons()) {
        if (s.cancel) {
            removePending(s.id, s.from);
            continue;
        }
        g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [&](const Pending& p) { return p.from == s.from; }),
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
        if (!s.hard) notices::push(s.name + " summons you");
    }
    // What happened to ours.
    for (const std::string& line : net::takeSummonReports()) notices::push(line);

    // Out of time (soft: they stop being offered a minute after they came), or already there.
    float me[3];
    const bool haveMe = here(me);
    for (size_t i = 0; i < g_pending.size();) {
        const Pending& p = g_pending[i];
        bool drop = !p.hard && now - p.at >= sp::kSoftWindowMs;
        if (haveMe) {
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

    // The send key.
    if (!in.canAct || !in.sendDown) {
        const bool tap = in.canAct && g_sendSince && !g_sendFired && !g_sendWithdraws && now - g_sendSince < kHoldMs;
        g_sendSince = 0;
        g_sendFired = false;
        g_sendProgress = 0.f;
        g_hintTarget = false; // the bar fades out with the progress it had
        if (tap) send({}, false);
    } else {
        if (!g_sendSince) {
            g_sendSince = now | 1;
            g_sendFired = false;
            g_sendWithdraws = net::summonOpen();
            g_hintCanHard = net::canHardSummon();
        }
        const DWORD held = now - g_sendSince;
        g_sendProgress = std::min(1.f, static_cast<float>(held) / static_cast<float>(kHoldMs));
        g_hintTarget = !g_sendFired && held >= kHintDelayMs;
        g_hintWithdraw = g_sendWithdraws;
        g_hintProgress = g_sendProgress;
        if (!g_sendFired && held >= kHoldMs) {
            g_sendFired = true;
            if (g_sendWithdraws) {
                net::cancelSummon();
                notices::push("Summon withdrawn");
            } else {
                send({}, true);
            }
        }
    }

    // The panel's keys: hold one for a second.
    const bool pendingShown = !g_pending.empty();
    const uint32_t topId = pendingShown ? g_pending.front().id : 0;
    if (topId != g_topId) { // a different summon is on top: keys held from before do not count
        g_topId = topId;
        g_accept.reset();
        g_decline.reset();
    }
    const uint32_t returnLeft = game::teleport::returnLeftMs();
    g_takeAccept = pendingShown;
    g_takeDecline = pendingShown || returnLeft > 0;
    g_acceptProgress = g_declineProgress = g_returnProgress = 0.f;
    if (!in.canAct) {
        g_accept.reset();
        g_decline.reset();
        g_return.reset();
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
    } else if (returnLeft > 0) {
        g_returnProgress = g_return.step(in.declineDown, now, kReturnHoldMs, done);
        if (done) {
            if (game::teleport::requestReturn()) notices::push("Going back");
            return;
        }
    } else {
        g_accept.reset();
        g_decline.reset();
        g_return.reset();
    }
}

void draw() {
    const auto cfg = config::get();
    const ImGuiIO& io = ImGui::GetIO();
    Panel pn{ImGui::GetForegroundDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), io.DisplaySize};
    const DWORD now = GetTickCount();
    if (!overlayHidden()) drawBeacons(pn);
    // The panel is a warning (a hard summon is about to move the player), so it is not hidden with the overlay.
    if (!g_pending.empty()) {
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
    if (const uint32_t left = game::teleport::returnLeftMs(); left > 0 && !game::teleport::busy())
        drawReturn(pn, left, keyHint(cfg->summonDeclineKey, cfg->summonDeclinePad));
    // The send key's hold: a bar near the middle of the screen. It shows only once the key has been held
    // for kHintDelayMs (a tap that places a plain summon never flashes it) and fades in and out over
    // kHintFadeMs.
    const float dt = std::clamp(io.DeltaTime, 0.f, 0.1f);
    g_hintAlpha = std::clamp(g_hintAlpha + (g_hintTarget ? dt : -dt) / (static_cast<float>(kHintFadeMs) / 1000.f), 0.f, 1.f);
    if (g_hintAlpha > 0.f) {
        const float k = g_hintAlpha;
        const auto fade = [k](int a) { return static_cast<int>(static_cast<float>(a) * k); };
        const char* what = g_hintWithdraw ? "Withdrawing the summon..."
                           : g_hintCanHard ? "Pulling players in..."
                                           : "Calling everyone (only the host can pull players in)";
        const float size = pn.size;
        const ImVec2 ts = pn.font->CalcTextSizeA(size, FLT_MAX, 0.f, what);
        const float pad = size * 0.5f;
        const ImVec2 a(pn.screen.x * 0.5f - ts.x * 0.5f - pad, pn.screen.y * 0.62f);
        const ImVec2 b(a.x + ts.x + 2 * pad, a.y + ts.y + pad);
        pn.dl->AddRectFilled(a, b, IM_COL32(12, 12, 18, fade(215)), size * 0.3f);
        pn.dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * g_hintProgress, b.y), IM_COL32(255, 110, 70, fade(200)), size * 0.3f);
        pn.dl->AddText(pn.font, size, ImVec2(a.x + pad, a.y + pad * 0.5f), IM_COL32(255, 255, 255, fade(255)), what);
    }
}

} // namespace mecmp::ui::summon
