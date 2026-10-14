#include "ui/markers.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "game/camera.h"
#include "game/camera_math.h"
#include "game/ghosts.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "ui/glyph.h"
#include "ui/overlay.h"

namespace mecmp::ui::markers {
namespace {
constexpr float kMarkerHeight = net::protocol::kPingLift; // a marker floats this far above its position
constexpr float kHeadHeight = 2.05f;  // as for name tags: above the ghost's feet
constexpr float kFadeLastMs = 3000.f; // a marker fades out over its last seconds (a third of its life if that is less)
// A marker is drawn at this fraction of its full size, and grows to the full size while the crosshair
// is on it (the one a double tap removes, or a quick answer goes to).
constexpr float kRestScale = 0.85f;
constexpr float kFocusScale = 1.10f;
constexpr float kGrowPerSecond = 9.f; // how fast it grows and shrinks: fully in about a ninth of a second
// The crosshair "is on" a marker when it is this many font sizes away from it on the screen.
constexpr float kFocusRadius = 3.0f;
// In "place under my feet" mode a marker of mine counts as the one at my feet within this distance.
constexpr float kFeetFocusMetres = net::protocol::kPingMergeMetres;
// A title that was moved off its marker gets a line back to it from this far on (font sizes).
constexpr float kLeaderFrom = 0.8f;
constexpr float kOffsetPerSecond = 14.f; // how fast a title glides to its place

// The focus: written by the render thread every frame, read by input threads.
std::mutex g_focusM;
Focus g_own, g_other;

ImU32 colour(const uint8_t rgb[3], float alpha) {
    return IM_COL32(rgb[0], rgb[1], rgb[2], static_cast<int>(255 * std::clamp(alpha, 0.f, 1.f)));
}

void label(ImDrawList* dl, ImFont* font, float size, ImVec2 centre, const char* text, float alpha, const ImVec2& screen) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    ImVec2 at(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f);
    at.x = std::clamp(at.x, 4.f, std::max(4.f, screen.x - ts.x - 4.f));
    at.y = std::clamp(at.y, 4.f, std::max(4.f, screen.y - ts.y - 4.f));
    dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, static_cast<int>(200 * alpha)), text);
    dl->AddText(font, size, at, IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)), text);
}

// A triangle on the screen's edge pointing along (dx, dy), with the text beside it towards the centre
// (and the marker's title, if any, a line above that).
void arrow(ImDrawList* dl, ImFont* font, const game::EdgeIndicator& e, float size, ImU32 fill, const char* text,
           const char* title, float alpha, const ImVec2& screen) {
    const ImVec2 tip(e.x + e.dirX * size, e.y + e.dirY * size);
    const ImVec2 back(e.x - e.dirX * size * 0.6f, e.y - e.dirY * size * 0.6f);
    const ImVec2 side(-e.dirY * size * 0.7f, e.dirX * size * 0.7f);
    const ImVec2 a(back.x + side.x, back.y + side.y), b(back.x - side.x, back.y - side.y);
    dl->AddTriangleFilled(tip, a, b, fill);
    dl->AddTriangle(tip, a, b, IM_COL32(0, 0, 0, static_cast<int>(200 * alpha)), 1.5f);
    const ImVec2 at(e.x - e.dirX * size * 2.6f, e.y - e.dirY * size * 2.2f);
    const float unit = ImGui::GetFontSize();
    label(dl, font, unit * 0.85f, at, text, alpha, screen);
    if (title && *title) label(dl, font, unit * 1.0f, ImVec2(at.x, at.y - unit * 1.0f), title, alpha, screen);
}

// One marker to draw on this frame.
struct Item {
    const net::PingInfo* ping = nullptr;
    game::EdgeIndicator e;
    float alpha = 0.f;
    float dist2 = 0.f;
    uint64_t key = 0;
    char text[96] = {};
    game::LabelBox box; // where its text goes (on screen markers)
    game::LabelBox glyph; // the icon: texts keep off it, its own and the others'
    ImVec2 off;         // how far the text is moved from there so that it does not cover another
    float scale = kRestScale;
};

uint64_t cellKey(const net::PingInfo& p) { return (static_cast<uint64_t>(p.owner) << 8) | p.index; }

Focus toFocus(const net::PingInfo& p) {
    Focus f;
    f.valid = true;
    f.owner = p.owner;
    f.index = p.index;
    f.name = p.name;
    f.label = p.label;
    return f;
}
} // namespace

Focus focusedOwn() {
    std::lock_guard<std::mutex> lock(g_focusM);
    return g_own;
}
Focus focusedOther() {
    std::lock_guard<std::mutex> lock(g_focusM);
    return g_other;
}

void draw() {
    const auto cfg = config::get();
    game::CameraView cam;
    if ((!cfg->showPings && !cfg->showArrows) || overlayHidden() || !game::camera::read(cam)) {
        std::lock_guard<std::mutex> lock(g_focusM);
        g_own = g_other = Focus{}; // nothing is shown, so nothing is under the crosshair
        return;
    }
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float unit = ImGui::GetFontSize();
    const float margin = unit * 2.2f;
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.f, 0.25f);
    char text[96];

    static std::unordered_map<uint64_t, float> grow;          // marker -> 0 (resting) .. 1 (focused); render thread only
    static std::unordered_map<uint64_t, ImVec2> glide;        // marker -> the title's offset as drawn
    std::unordered_map<uint64_t, float> growNow;
    std::unordered_map<uint64_t, ImVec2> glideNow;
    Focus own, other;

    if (cfg->showPings) {
        auto pings = net::pings();
        std::vector<Item> items;
        items.reserve(pings.size());
        for (const auto& p : pings) {
            Item it;
            it.ping = &p;
            const float at[3] = {p.pos[0], p.pos[1] + kMarkerHeight, p.pos[2]};
            if (!game::edgeIndicator(cam, at, screen.x, screen.y, margin, it.e)) continue;
            // Fades with distance (later than the names do), and over its last seconds.
            const float fadeMs = std::min(kFadeLastMs, static_cast<float>(p.lifeMs) / 3.f);
            it.alpha = game::distanceFade(it.e.distance, game::kMarkerFadeFullUntil, game::kMarkerFadeGoneAt) *
                       std::clamp(static_cast<float>(p.leftMs) / fadeMs, 0.f, 1.f);
            if (it.alpha <= 0.f) continue;
            float d2 = 0.f;
            for (int i = 0; i < 3; ++i) d2 += (p.pos[i] - cam.pos[i]) * (p.pos[i] - cam.pos[i]);
            it.dist2 = d2;
            it.key = cellKey(p);
            items.push_back(it);
        }
        // Farthest first, so a marker in front is drawn over the ones behind it.
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist2 > b.dist2; });

        // Which one the crosshair is on: the nearest to the middle of the screen, among those on screen
        // (separately for mine and for the others'). In "under my feet" mode mine is the one at my feet.
        {
            std::vector<game::ScreenPoint> pts[2]; // [0] others, [1] own
            std::vector<int> idx[2];
            for (size_t i = 0; i < items.size(); ++i) {
                if (!items[i].e.onScreen) continue;
                const int k = items[i].ping->own ? 1 : 0;
                pts[k].push_back({items[i].e.x, items[i].e.y});
                idx[k].push_back(static_cast<int>(i));
            }
            const float cx = screen.x * 0.5f, cy = screen.y * 0.5f, radius = unit * kFocusRadius;
            const int o = game::nearestToCentre(pts[0].data(), static_cast<int>(pts[0].size()), cx, cy, radius);
            if (o >= 0) other = toFocus(*items[idx[0][o]].ping);
            if (cfg->pingAim) {
                const int m = game::nearestToCentre(pts[1].data(), static_cast<int>(pts[1].size()), cx, cy, radius);
                if (m >= 0) own = toFocus(*items[idx[1][m]].ping);
            } else {
                static net::PlayerState local; // large; render thread only
                if (net::localState(local)) {
                    float best = kFeetFocusMetres * kFeetFocusMetres;
                    for (const auto& it : items) {
                        if (!it.ping->own) continue;
                        float d2 = 0.f;
                        for (int i = 0; i < 3; ++i) d2 += (it.ping->pos[i] - local.pos[i]) * (it.ping->pos[i] - local.pos[i]);
                        if (d2 <= best) {
                            best = d2;
                            own = toFocus(*it.ping);
                        }
                    }
                }
            }
        }

        // Size: resting, or grown while focused.
        for (auto& it : items) {
            const bool focused = (own.valid && own.owner == it.ping->owner && own.index == it.ping->index && it.ping->own) ||
                                 (other.valid && other.owner == it.ping->owner && other.index == it.ping->index && !it.ping->own);
            const auto was = grow.find(it.key);
            float t = was == grow.end() ? 0.f : was->second;
            t = std::clamp(t + (focused ? 1.f : -1.f) * kGrowPerSecond * dt, 0.f, 1.f);
            growNow[it.key] = t;
            it.scale = kRestScale + (kFocusScale - kRestScale) * t;
        }

        // Where each marker's text goes, and what to move so that texts do not cover each other.
        const float lineH = unit * 0.85f;
        std::vector<int> byNear;
        for (size_t i = 0; i < items.size(); ++i) {
            Item& it = items[i];
            snprintf(it.text, sizeof(it.text), "%s  %.0f m", it.ping->name.c_str(), it.e.distance);
            if (!it.e.onScreen) continue;
            const game::Pulse pulse = cfg->pingPulse ? game::pingPulse(it.ping->ageMs) : game::Pulse{};
            const float r = unit * std::clamp(1.1f - it.e.distance / 120.f, 0.55f, 1.1f) * it.scale * pulse.scale;
            const float top = it.e.y - r - unit * 0.6f;
            const ImVec2 a = font->CalcTextSizeA(lineH, FLT_MAX, 0.f, it.text);
            float w = a.x, h = a.y;
            if (!it.ping->label.empty()) {
                const ImVec2 t = font->CalcTextSizeA(unit * 1.05f, FLT_MAX, 0.f, it.ping->label.c_str());
                w = std::max(w, t.x);
                h += unit * 1.0f;
            }
            it.box = {it.e.x - w * 0.5f, top - a.y * 0.5f - (it.ping->label.empty() ? 0.f : unit * 1.0f), w, h};
            const float gr = r * 0.9f; // a little less than drawn: a title sits just above its own icon
            it.glyph = {it.e.x - gr, it.e.y - gr, gr * 2.f, gr * 2.f};
            byNear.push_back(static_cast<int>(i));
        }
        // Nearest first, in steps of 2 m, and by marker within a step: two markers about as far away do
        // not trade places from one frame to the next (their titles would jump).
        std::sort(byNear.begin(), byNear.end(), [&items](int a, int b) {
            const float qa = std::floor(std::sqrt(items[a].dist2) * 0.5f), qb = std::floor(std::sqrt(items[b].dist2) * 0.5f);
            if (qa != qb) return qa < qb;
            return items[a].key < items[b].key;
        });
        // The icons first (they stay), then the titles from the nearest marker out; a title that would
        // cover an icon or a nearer title moves up, above it.
        std::vector<game::LabelBox> boxes;
        boxes.reserve(byNear.size() * 2);
        for (int i : byNear) boxes.push_back(items[i].glyph);
        const int fixedBoxes = static_cast<int>(boxes.size());
        for (int i : byNear) boxes.push_back(items[i].box);
        game::stackLabelsUp(boxes.data(), static_cast<int>(boxes.size()), fixedBoxes, unit * 0.15f);
        for (size_t k = 0; k < byNear.size(); ++k) {
            Item& it = items[byNear[k]];
            const game::LabelBox& placed = boxes[static_cast<size_t>(fixedBoxes) + k];
            const ImVec2 want(placed.x - it.box.x, placed.y - it.box.y);
            const auto was = glide.find(it.key);
            ImVec2 now = was == glide.end() ? want : was->second;
            const float f = std::clamp(kOffsetPerSecond * dt, 0.f, 1.f);
            now.x += (want.x - now.x) * f;
            now.y += (want.y - now.y) * f;
            glideNow[it.key] = now;
            it.off = now;
        }

        for (const auto& it : items) {
            const net::PingInfo& p = *it.ping;
            const game::EdgeIndicator& e = it.e;
            // A flash when it arrives, so a new or moved marker is noticed (unless switched off).
            const game::Pulse pulse = cfg->pingPulse ? game::pingPulse(p.ageMs) : game::Pulse{};
            // During the blinks the marker's colours go towards white.
            uint8_t lit[3], litAccent[3];
            for (int i = 0; i < 3; ++i) {
                lit[i] = static_cast<uint8_t>(p.rgb[i] + (255 - p.rgb[i]) * pulse.flash);
                litAccent[i] = static_cast<uint8_t>(p.accent[i] + (255 - p.accent[i]) * pulse.flash);
            }
            const float alpha = it.alpha;
            if (e.onScreen) {
                const float full = unit * std::clamp(1.1f - e.distance / 120.f, 0.55f, 1.1f);
                const float r = full * it.scale;
                if (pulse.ring < 1.f) {
                    // Two rings: a thick one close in and a thinner one beyond it. Both thin out and
                    // lose their colour as they spread.
                    const float fade = (1.f - pulse.ring) * alpha;
                    const ImVec2 c(e.x, e.y);
                    dl->AddCircle(c, r * (1.f + 1.8f * pulse.ring), colour(lit, fade), 0, unit * 0.32f * (1.f - 0.6f * pulse.ring));
                    dl->AddCircle(c, r * (1.f + 3.4f * pulse.ring), colour(lit, fade * 0.75f), 0, unit * 0.14f * (1.f - 0.5f * pulse.ring));
                }
                drawGlyph(dl, p.glyph, ImVec2(e.x, e.y), r * pulse.scale, colour(lit, alpha), alpha, colour(litAccent, alpha));
                // The text goes above the marker, which points at something: below it would cover that.
                // Name and distance sit on it, the marker's title (if it has one) above them. When two
                // markers' texts would cover each other the farther one's moves aside (a line leads back).
                const float top = e.y - r * pulse.scale - unit * 0.6f;
                const ImVec2 at(e.x + it.off.x, top + it.off.y);
                const float offLen = std::sqrt(it.off.x * it.off.x + it.off.y * it.off.y);
                if (offLen > unit * kLeaderFrom) {
                    const float cy = std::clamp(e.y - r * pulse.scale, it.box.y + it.off.y, it.box.y + it.off.y + it.box.h);
                    dl->AddLine(ImVec2(e.x, e.y - r * pulse.scale), ImVec2(std::clamp(e.x, it.box.x + it.off.x, it.box.x + it.off.x + it.box.w), cy),
                                IM_COL32(255, 255, 255, static_cast<int>(110 * alpha)), 1.f);
                }
                label(dl, font, unit * 0.85f, at, it.text, alpha, screen);
                if (!p.label.empty()) label(dl, font, unit * 1.05f, ImVec2(at.x, at.y - unit * 1.0f), p.label.c_str(), alpha, screen);
            } else {
                arrow(dl, font, e, unit * 0.7f * pulse.scale, colour(lit, alpha), it.text, p.label.c_str(), alpha, screen);
            }
        }
    }
    grow.swap(growNow);
    glide.swap(glideNow);
    {
        std::lock_guard<std::mutex> lock(g_focusM);
        g_own = own;
        g_other = other;
    }

    if (cfg->showArrows) {
        const auto shown = game::ghosts::shownPeers();
        if (shown.empty()) return;
        static net::PlayerState st; // large; render thread only
        const uint8_t white[3] = {255, 255, 255};
        for (const auto& r : net::remotes(5000)) {
            if (std::find(shown.begin(), shown.end(), r.id) == shown.end()) continue;
            uint32_t age = 0;
            if (!net::remoteState(r.id, st, age)) continue;
            const float head[3] = {st.pos[0], st.pos[1] + kHeadHeight, st.pos[2]};
            game::EdgeIndicator e;
            // "Off screen" only: a player hidden behind a wall but inside the view gets no arrow.
            if (!game::edgeIndicator(cam, head, screen.x, screen.y, margin, e) || e.onScreen) continue;
            // Faded with distance like markers: no arrows to players across the map (or from the map
            // screen, whose camera sits kilometres away).
            const float a = 0.9f * game::distanceFade(e.distance, game::kMarkerFadeFullUntil, game::kMarkerFadeGoneAt);
            if (a <= 0.f) continue;
            snprintf(text, sizeof(text), "%s  %.0f m", r.name.empty() ? "Runner" : r.name.c_str(), e.distance);
            arrow(dl, font, e, unit * 0.55f, colour(white, a), text, nullptr, a, screen);
        }
    }
}

} // namespace mecmp::ui::markers
