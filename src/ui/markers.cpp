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

namespace mecmp::ui::markers {
namespace {
constexpr float kMarkerHeight = 1.2f; // a marker floats this far above the spot it was placed on
constexpr float kHeadHeight = 2.05f;  // as for name tags: above the ghost's feet
constexpr float kFadeLastMs = 3000.f; // a marker fades out over its last seconds

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

// A triangle on the screen's edge pointing along (dx, dy), with the text beside it towards the centre.
void arrow(ImDrawList* dl, ImFont* font, const game::EdgeIndicator& e, float size, ImU32 fill, const char* text, float alpha,
           const ImVec2& screen) {
    const ImVec2 tip(e.x + e.dirX * size, e.y + e.dirY * size);
    const ImVec2 back(e.x - e.dirX * size * 0.6f, e.y - e.dirY * size * 0.6f);
    const ImVec2 side(-e.dirY * size * 0.7f, e.dirX * size * 0.7f);
    const ImVec2 a(back.x + side.x, back.y + side.y), b(back.x - side.x, back.y - side.y);
    dl->AddTriangleFilled(tip, a, b, fill);
    dl->AddTriangle(tip, a, b, IM_COL32(0, 0, 0, static_cast<int>(200 * alpha)), 1.5f);
    label(dl, font, ImGui::GetFontSize() * 0.85f, ImVec2(e.x - e.dirX * size * 2.6f, e.y - e.dirY * size * 2.2f), text, alpha, screen);
}

void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, float alpha) {
    const ImVec2 top(c.x, c.y - r), right(c.x + r * 0.7f, c.y), bottom(c.x, c.y + r), left(c.x - r * 0.7f, c.y);
    dl->AddQuadFilled(top, right, bottom, left, fill);
    dl->AddQuad(top, right, bottom, left, IM_COL32(0, 0, 0, static_cast<int>(220 * alpha)), 1.5f);
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
        for (const auto& p : net::pings()) {
            const float at[3] = {p.pos[0], p.pos[1] + kMarkerHeight, p.pos[2]};
            game::EdgeIndicator e;
            if (!game::edgeIndicator(cam, at, screen.x, screen.y, margin, e)) continue;
            // Fades with distance like the names, and over its last seconds.
            const float left = static_cast<float>(net::protocol::kPingLifeMs) - static_cast<float>(p.ageMs);
            const float alpha = game::distanceFade(e.distance) * std::clamp(left / kFadeLastMs, 0.f, 1.f);
            if (alpha <= 0.f) continue;
            snprintf(text, sizeof(text), "%s  %.0f m", p.name.c_str(), e.distance);
            if (e.onScreen) {
                const float r = unit * std::clamp(1.1f - e.distance / 120.f, 0.55f, 1.1f);
                diamond(dl, ImVec2(e.x, e.y), r, colour(p.rgb, alpha), alpha);
                label(dl, font, unit * 0.85f, ImVec2(e.x, e.y + r + unit * 0.6f), text, alpha, screen);
            } else {
                arrow(dl, font, e, unit * 0.7f, colour(p.rgb, alpha), text, alpha, screen);
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
            arrow(dl, font, e, unit * 0.55f, colour(white, 0.9f), text, 0.9f, screen);
        }
    }
}

} // namespace mecmp::ui::markers
