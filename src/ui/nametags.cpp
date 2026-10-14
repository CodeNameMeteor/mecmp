#include "ui/nametags.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include "core/config.h"
#include "game/camera.h"
#include "game/ghosts.h"
#include "game/hooks.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "ui/overlay.h"

namespace mecmp::ui::nametags {
namespace {
constexpr float kHeadHeight = 2.05f; // above the ghost's trajectory bone (its feet)

// A tag line in a dark rounded box, centred on x, with its bottom at `bottom`; returns its top.
float tagLine(ImDrawList* dl, ImFont* font, float size, float x, float bottom, const char* text, ImU32 colour, float alpha) {
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text);
    const ImVec2 at(x - ts.x * 0.5f, bottom - ts.y);
    dl->AddRectFilled(ImVec2(at.x - size * 0.3f, at.y - size * 0.1f), ImVec2(at.x + ts.x + size * 0.3f, at.y + ts.y + size * 0.1f),
                      IM_COL32(0, 0, 0, static_cast<int>(90 * alpha)), size * 0.25f);
    dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, static_cast<int>(200 * alpha)), text);
    dl->AddText(font, size, at, colour, text);
    return at.y - size * 0.25f;
}

// "Dash: The View  0:12.4" (tenths: the clock is the sender's, a packet a second, counted on here).
std::string runLine(const net::RemoteInfo& r) {
    const uint32_t tenths = r.runMs / 100;
    char clock[32];
    if (tenths >= 600) snprintf(clock, sizeof(clock), "%u:%02u.%u", tenths / 600, (tenths / 10) % 60, tenths % 10);
    else snprintf(clock, sizeof(clock), "%u.%u", tenths / 10, tenths % 10);
    std::string s = net::protocol::runKindName(r.runKind);
    if (!r.runTitle.empty()) s += ": " + r.runTitle;
    return s + "  " + clock;
}
} // namespace

void draw() {
    if (!config::get()->showNames || overlayHidden() || game::hooks::gameMenuOpen()) return; // the map has its own camera
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
        const float alpha = game::distanceFade(depth);
        if (alpha <= 0.f) continue;
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
        // Above the name: the run they are in, with its clock, and "typing..." while their chat box is open.
        float top = at.y - size * 0.35f;
        const float small = size * 0.85f;
        if (r.runKind != net::protocol::kActivityNone)
            top = tagLine(dl, font, small, sx, top, runLine(r).c_str(), IM_COL32(255, 210, 60, static_cast<int>(255 * alpha)), alpha);
        if (r.typing) {
            static const char* const kDots[] = {"typing", "typing.", "typing..", "typing..."};
            const char* t = kDots[(GetTickCount() / 350) % 4];
            tagLine(dl, font, small, sx, top, t, IM_COL32(180, 220, 255, static_cast<int>(230 * alpha)), alpha);
        }
    }
}

} // namespace mecmp::ui::nametags
