#include "ui/overlay.h"
#include "ui/chat.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/config.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"

namespace mecmp::ui::chat {
namespace {
constexpr size_t kShownLines = 8;
constexpr DWORD kShowMs = 12000; // a line stays this long...
constexpr DWORD kFadeMs = 2000;  // ...then fades out (while typing, every line is shown)
std::atomic<bool> g_open{false};
std::atomic<bool> g_focus{false}; // put the cursor into the input on the next frame
char g_text[net::protocol::kMaxChatBytes * 2] = {};
} // namespace

void open() {
    g_text[0] = 0;
    g_focus = true;
    g_open = true;
}
void close() { g_open = false; }
bool isOpen() { return g_open.load(); }

void draw() {
    const bool typing = g_open.load();
    if (!typing && !config::get()->showChat) return;
    const auto lines = net::chatLines(kShownLines);
    const DWORD now = GetTickCount();
    const ImGuiIO& io = ImGui::GetIO();
    const float pad = ImGui::GetFontSize() * 0.5f;
    const float width = ImGui::GetFontSize() * 28;
    const float inputH = typing ? ImGui::GetFrameHeight() + pad : 0.f;
    float y = io.DisplaySize.y * 0.78f - inputH;

    // The lines, newest at the bottom, drawn without a window (they never take input).
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!typing && overlayHidden()) break;
        const DWORD age = now - it->atMs;
        float alpha = 1.f;
        if (!typing) {
            if (age >= kShowMs + kFadeMs) continue;
            if (age > kShowMs) alpha = 1.f - static_cast<float>(age - kShowMs) / kFadeMs;
        }
        const std::string who = it->name + ": ";
        const ImVec2 nameSize = font->CalcTextSizeA(size, FLT_MAX, 0.f, who.c_str());
        const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, width - nameSize.x, it->text.c_str());
        y -= textSize.y + pad * 0.4f;
        const ImVec2 at(pad * 2, y);
        dl->AddRectFilled(ImVec2(at.x - pad * 0.5f, at.y - pad * 0.2f),
                          ImVec2(at.x + nameSize.x + textSize.x + pad * 0.5f, at.y + textSize.y + pad * 0.2f),
                          ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.07f, 0.55f * alpha)), size * 0.25f);
        const ImVec4 nameColour = it->self ? ImVec4(0.55f, 0.8f, 1.f, alpha) : ImVec4(1.f, 0.8f, 0.4f, alpha);
        dl->AddText(font, size, at, ImGui::GetColorU32(nameColour), who.c_str());
        dl->AddText(font, size, ImVec2(at.x + nameSize.x, at.y), ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), it->text.c_str(),
                    nullptr, width - nameSize.x);
    }

    if (!typing) return;
    ImGui::SetNextWindowPos(ImVec2(pad * 1.5f, io.DisplaySize.y * 0.78f - inputH));
    ImGui::SetNextWindowSize(ImVec2(width + pad, 0));
    ImGui::SetNextWindowBgAlpha(0.7f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("###mecmp_chat", nullptr, flags)) {
        if (g_focus.exchange(false)) {
            ImGui::SetWindowFocus();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1);
        const bool sent = ImGui::InputTextWithHint("##say", "Say something (Enter to send, Esc to cancel)", g_text,
                                                   net::protocol::kMaxChatBytes + 1, ImGuiInputTextFlags_EnterReturnsTrue);
        if (sent) {
            net::sendChat(g_text);
            g_open = false;
        }
    }
    ImGui::End();
}

} // namespace mecmp::ui::chat
