#include "ui/nametags.h"

#include <algorithm>
#include <cfloat>

#include "core/config.h"
#include "game/camera.h"
#include "game/ghosts.h"
#include "imgui.h"
#include "net/net.h"

namespace mecmp::ui::nametags {
namespace {
constexpr float kHeadHeight = 2.05f; // above the ghost's trajectory bone (its feet)
constexpr float kFullUntil = 40.f;   // metres: fully visible up to here...
constexpr float kGoneAt = 150.f;     // ...then fading out until here
} // namespace

void draw() {
    if (!config::get()->showNames) return;
    const auto shown = game::ghosts::shownPeers();
    if (shown.empty()) return;
    game::CameraView cam;
    if (!game::camera::read(cam)) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    static net::PlayerState st; // large; render thread only
    for (const auto& r : net::remotes(5000)) {
        if (std::find(shown.begin(), shown.end(), r.id) == shown.end()) continue;
        uint32_t age = 0;
        if (!net::remoteState(r.id, st, age)) continue;
        const float head[3] = {st.pos[0], st.pos[1] + kHeadHeight, st.pos[2]};
        float sx = 0, sy = 0, depth = 0;
        if (!game::projectToScreen(cam, head, io.DisplaySize.x, io.DisplaySize.y, sx, sy, depth)) continue;
        if (depth > kGoneAt) continue;
        const float alpha = depth <= kFullUntil ? 1.f : 1.f - (depth - kFullUntil) / (kGoneAt - kFullUntil);
        const float size = ImGui::GetFontSize() * std::clamp(1.2f - depth / 60.f, 0.75f, 1.2f);
        const std::string name = r.name.empty() ? std::string("Runner") : r.name;
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, name.c_str());
        const ImVec2 at(sx - ts.x * 0.5f, sy - ts.y);
        if (at.x > io.DisplaySize.x || at.y > io.DisplaySize.y || at.x + ts.x < 0 || at.y + ts.y < 0) continue;
        const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(200 * alpha));
        const ImU32 text = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));
        dl->AddRectFilled(ImVec2(at.x - size * 0.3f, at.y - size * 0.1f), ImVec2(at.x + ts.x + size * 0.3f, at.y + ts.y + size * 0.1f),
                          IM_COL32(0, 0, 0, static_cast<int>(90 * alpha)), size * 0.25f);
        dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), shadow, name.c_str());
        dl->AddText(font, size, at, text, name.c_str());
    }
}

} // namespace mecmp::ui::nametags
