#include "ui/notices.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <deque>
#include <string>
#include <unordered_map>

#include "core/config.h"
#include "imgui.h"
#include "net/net.h"
#include "ui/overlay.h"

namespace mecmp::ui::notices {
namespace {
constexpr DWORD kShowMs = 5000;   // each notice stays this long...
constexpr DWORD kFadeMs = 1000;   // ...fading out over the last second
constexpr size_t kMaxShown = 4;

struct Notice { std::string text; DWORD at; };
std::deque<Notice> g_notices;
std::unordered_map<uint64_t, uint32_t> g_pingAge;    // owner and cell -> age of that marker when last looked at
} // namespace

void push(std::string text) {
    g_notices.push_back({std::move(text), GetTickCount()});
    while (g_notices.size() > kMaxShown) g_notices.pop_front();
}

void draw() {
    const bool on = config::get()->showNotices;
    for (const auto& text : net::takeConnectionNotices())
        if (on) push(text);

    // Other players' markers. A marker counts as new when its cell was empty (and it is fresh: not
    // one that was already there when we joined), or when its age went back (a replacement starts
    // again at 0; the same marker placed again right away does not). Tracked while notices are off
    // too, like joins above.
    constexpr uint32_t kFreshMs = 3000;
    std::unordered_map<uint64_t, uint32_t> pingAge;
    for (const auto& p : net::pings()) {
        if (p.own) continue;
        const uint64_t cell = (static_cast<uint64_t>(p.owner) << 8) | p.index;
        pingAge[cell] = p.ageMs;
        const auto was = g_pingAge.find(cell);
        const bool isNew = was == g_pingAge.end() ? p.ageMs < kFreshMs : p.ageMs < was->second;
        if (on && isNew)
            push((p.name.empty() ? std::string("Someone") : p.name) + " placed a marker" +
                 (p.label.empty() ? std::string() : ": " + p.label));
    }
    g_pingAge.swap(pingAge);

    // Quick answers to my markers: `Foie reacted "On my way" to your "Here" marker`.
    for (const auto& re : net::takeReactions())
        if (on)
            push(re.from + " reacted \"" + re.text + "\" to your " +
                 (re.title.empty() ? std::string("marker") : "\"" + re.title + "\" marker"));

    // Others' finished runs: "Foie finished the dash The View in 1:23.45 (new record)".
    for (const auto& text : net::takeResults())
        if (on) push(text);

    const DWORD t = GetTickCount();
    while (!g_notices.empty() && t - g_notices.front().at >= kShowMs) g_notices.pop_front();
    if (g_notices.empty() || !on || overlayHidden()) return;

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
