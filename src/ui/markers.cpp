#include "ui/markers.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include "core/config.h"
#include "game/camera.h"
#include "game/ghosts.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "ui/glyph.h"

namespace mecmp::ui::markers {
namespace {
constexpr float kMarkerHeight = net::protocol::kPingLift; // a marker floats this far above its position
constexpr float kHeadHeight = 2.05f;  // as for name tags: above the ghost's feet
constexpr float kFadeLastMs = 3000.f; // a marker fades out over its last seconds (a third of its life if that is less)

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
} // namespace

void draw() {
    const auto cfg = config::get();
    if (!cfg->showPings && !cfg->showArrows) return;
    game::CameraView cam;
    if (!game::camera::read(cam)) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float unit = ImGui::GetFontSize();
    const float margin = unit * 2.2f;
    char text[96];

    if (cfg->showPings) {
        // Farthest first, so a marker in front is drawn over the ones behind it.
        auto pings = net::pings();
        auto away = [&cam](const net::PingInfo& p) {
            float d2 = 0.f;
            for (int i = 0; i < 3; ++i) d2 += (p.pos[i] - cam.pos[i]) * (p.pos[i] - cam.pos[i]);
            return d2;
        };
        std::stable_sort(pings.begin(), pings.end(), [&away](const net::PingInfo& a, const net::PingInfo& b) { return away(a) > away(b); });
        for (const auto& p : pings) {
            const float at[3] = {p.pos[0], p.pos[1] + kMarkerHeight, p.pos[2]};
            game::EdgeIndicator e;
            if (!game::edgeIndicator(cam, at, screen.x, screen.y, margin, e)) continue;
            // Fades with distance (later than the names do), and over its last seconds.
            const float fadeMs = std::min(kFadeLastMs, static_cast<float>(p.lifeMs) / 3.f);
            const float alpha = game::distanceFade(e.distance, game::kMarkerFadeFullUntil, game::kMarkerFadeGoneAt) * std::clamp(static_cast<float>(p.leftMs) / fadeMs, 0.f, 1.f);
            if (alpha <= 0.f) continue;
            snprintf(text, sizeof(text), "%s  %.0f m", p.name.c_str(), e.distance);
            // A flash when it arrives, so a new or moved marker is noticed (unless switched off).
            const game::Pulse pulse = cfg->pingPulse ? game::pingPulse(p.ageMs) : game::Pulse{};
            // During the blinks the marker's colours go towards white.
            uint8_t lit[3], litAccent[3];
            for (int i = 0; i < 3; ++i) {
                lit[i] = static_cast<uint8_t>(p.rgb[i] + (255 - p.rgb[i]) * pulse.flash);
                litAccent[i] = static_cast<uint8_t>(p.accent[i] + (255 - p.accent[i]) * pulse.flash);
            }
            if (e.onScreen) {
                const float r = unit * std::clamp(1.1f - e.distance / 120.f, 0.55f, 1.1f);
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
                // Name and distance sit on it, the marker's title (if it has one) above them.
                const float top = e.y - r * pulse.scale - unit * 0.6f;
                label(dl, font, unit * 0.85f, ImVec2(e.x, top), text, alpha, screen);
                if (!p.label.empty()) label(dl, font, unit * 1.05f, ImVec2(e.x, top - unit * 1.0f), p.label.c_str(), alpha, screen);
            } else {
                arrow(dl, font, e, unit * 0.7f * pulse.scale, colour(lit, alpha), text, p.label.c_str(), alpha, screen);
            }
        }
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
            snprintf(text, sizeof(text), "%s  %.0f m", r.name.empty() ? "Runner" : r.name.c_str(), e.distance);
            arrow(dl, font, e, unit * 0.55f, colour(white, 0.9f), text, nullptr, 0.9f, screen);
        }
    }
}

} // namespace mecmp::ui::markers
