#include "ui/notices.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>

#include "core/config.h"
#include "imgui.h"
#include "net/net.h"

namespace mecmp::ui::notices {
namespace {
constexpr DWORD kShowMs = 5000;   // each notice stays this long...
constexpr DWORD kFadeMs = 1000;   // ...fading out over the last second
constexpr size_t kMaxShown = 4;
constexpr uint32_t kGoneMs = 5000; // same as the ghosts: silent this long = left

struct Notice { std::string text; DWORD at; };
std::deque<Notice> g_notices;
std::unordered_map<uint32_t, std::string> g_present; // id -> name, players announced as here

void push(std::string text) {
    g_notices.push_back({std::move(text), GetTickCount()});
    while (g_notices.size() > kMaxShown) g_notices.pop_front();
}
} // namespace

void draw() {
    // Track arrivals and departures even while notices are off, so turning them on does not
    // announce everyone at once.
    std::unordered_map<uint32_t, std::string> now;
    for (const auto& r : net::remotes(kGoneMs))
        if (!r.loopback && !r.name.empty() && r.ageMs < kGoneMs) now[r.id] = r.name; // named: their HELLO arrived
    const bool on = config::get()->showNotices;
    for (const auto& kv : now)
        if (!g_present.count(kv.first) && on) push(kv.second + " joined");
    for (const auto& kv : g_present)
        if (!now.count(kv.first) && on) push(kv.second + " left");
    g_present.swap(now);

    const DWORD t = GetTickCount();
    while (!g_notices.empty() && t - g_notices.front().at >= kShowMs) g_notices.pop_front();
    if (g_notices.empty() || !on) return;

    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float pad = size * 0.45f;
    float y = io.DisplaySize.y * 0.06f;
    for (const auto& n : g_notices) {
        const DWORD age = t - n.at;
        const float alpha = age + kFadeMs > kShowMs ? static_cast<float>(kShowMs - age) / kFadeMs : 1.f;
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, n.text.c_str());
        const ImVec2 a(io.DisplaySize.x * 0.5f - ts.x * 0.5f - pad, y);
        const ImVec2 b(a.x + ts.x + 2 * pad, y + ts.y + pad);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.07f, 0.7f * alpha)), size * 0.3f);
        dl->AddText(font, size, ImVec2(a.x + pad, a.y + pad * 0.5f), ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), n.text.c_str());
        y = b.y + pad * 0.5f;
    }
}

} // namespace mecmp::ui::notices
