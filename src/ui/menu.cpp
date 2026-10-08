#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/menu.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "app/mod.h"
#include "core/config.h"
#include "game/ghosts.h"
#include "game/models.h"
#include "imgui.h"
#include "net/net.h"

namespace mecmp::ui::menu {
namespace {

char g_name[25] = {};
char g_code[64] = {};
bool g_inited = false;
DWORD g_hintUntil = 0;

void copyToClipboard(const std::string& text) {
    if (text.empty() || !OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

std::string fromClipboard() {
    std::string s;
    if (!OpenClipboard(nullptr)) return s;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* p = static_cast<const char*>(GlobalLock(h))) s = p;
        GlobalUnlock(h);
    }
    CloseClipboard();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string shortModel(const std::string& m) {
    const size_t slash = m.find_last_of('/');
    std::string s = slash == std::string::npos ? m : m.substr(slash + 1);
    if (s.rfind("Customization_", 0) == 0) s = s.substr(14);
    if (s.rfind("C_", 0) == 0) s = s.substr(2);
    if (s == "faith") s = "Faith (your outfit)";
    return s;
}

void startSession(const std::string& join) {
    auto& cfg = config::mut();
    cfg.name = g_name[0] ? g_name : "Runner";
    cfg.join = join;
    net::start(cfg.port, cfg.join, cfg.loopbackOffset, cfg.name);
    config::save();
}

void sessionSection() {
    ImGui::SeparatorText("Session");
    const bool running = net::running();
    if (!running) {
        ImGui::TextDisabled("Offline");
    } else if (net::hosting()) {
        const std::string code = net::joinCode();
        if (code.empty()) {
            ImGui::Text("Hosting. Preparing your join code...");
        } else {
            ImGui::Text("Hosting. Your join code:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "%s", code.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) copyToClipboard(code);
        }
        const std::string lan = net::lanCode();
        if (!lan.empty()) {
            ImGui::TextDisabled("Same network: %s", lan.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy##lan")) copyToClipboard(lan);
        }
    } else {
        ImGui::Text("Joined %s", config::get().join.c_str());
        ImGui::SameLine();
        if (net::connectedToHost()) ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "connected");
        else ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "waiting for the host...");
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputText("Your name", g_name, sizeof(g_name));
    ImGui::SameLine();
    if (ImGui::Button("Set")) {
        config::mut().name = g_name[0] ? g_name : "Runner";
        net::setName(config::get().name);
        config::save();
    }

    ImGui::Spacing();
    if (ImGui::Button("Host a session")) startSession("");
    ImGui::SameLine();
    ImGui::TextDisabled("(friends join with your code)");

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputTextWithHint("##code", "join code", g_code, sizeof(g_code), ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if (ImGui::Button("Paste")) snprintf(g_code, sizeof(g_code), "%s", fromClipboard().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(g_code[0] == 0);
    if (ImGui::Button("Join")) startSession(g_code);
    ImGui::EndDisabled();
    if (ImGui::Button("Solo test (mirror me)")) startSession("127.0.0.1:" + std::to_string(config::get().port));
}

void playersSection() {
    ImGui::SeparatorText("Players");
    const auto remotes = net::remotes(5000);
    if (remotes.empty()) {
        ImGui::TextDisabled("Nobody else here yet.");
        return;
    }
    if (ImGui::BeginTable("players", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& r : remotes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", r.name.empty() ? "(joining...)" : r.name.c_str(), r.loopback ? " (you, mirrored)" : "");
            ImGui::TableNextColumn();
            if (r.ageMs < 1000) ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "live");
            else ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%.1f s ago", r.ageMs / 1000.f);
        }
        ImGui::EndTable();
    }
}

void lookSection() {
    ImGui::SeparatorText("How other players look");
    const auto names = game::models::names();
    const std::string& cur = config::get().model;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::BeginCombo("Model", shortModel(cur).c_str())) {
        for (const auto& n : names) {
            if (ImGui::Selectable(shortModel(n).c_str(), n == cur)) {
                game::ghosts::requestModel(n);
                config::mut().model = n;
                config::save();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Show ghosts")) game::ghosts::requestSpawnOrRefresh();
    ImGui::SameLine();
    if (ImGui::Button("Hide ghosts")) game::ghosts::requestHideAll();
}
} // namespace

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.f;
    st.FrameRounding = 4.f;
    st.GrabRounding = 4.f;
    st.WindowPadding = ImVec2(14, 12);
    st.ItemSpacing = ImVec2(8, 7);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.07f, 0.94f);
    c[ImGuiCol_Button] = ImVec4(0.78f, 0.13f, 0.13f, 0.75f); // Catalyst red
    c[ImGuiCol_ButtonHovered] = ImVec4(0.90f, 0.20f, 0.20f, 0.90f);
    c[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.28f, 0.28f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.78f, 0.13f, 0.13f, 0.45f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.90f, 0.20f, 0.20f, 0.70f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.55f, 0.08f, 0.08f, 1.f);
}

void draw(bool open) {
    if (!g_inited) {
        g_inited = true;
        snprintf(g_name, sizeof(g_name), "%s", config::get().name.c_str());
        snprintf(g_code, sizeof(g_code), "%s", config::get().join.c_str());
        g_hintUntil = GetTickCount() + 10000;
    }
    if (!open) {
        // A short hint after start-up, top-left.
        if (GetTickCount() < g_hintUntil) {
            ImGui::SetNextWindowPos(ImVec2(16, 16));
            ImGui::SetNextWindowBgAlpha(0.6f);
            ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
            ImGui::Text("MECMP %s: press %s for the multiplayer menu", app::kVersion, config::get().menuKey.c_str());
            ImGui::End();
        }
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_FirstUseEver);
    char title[64];
    snprintf(title, sizeof(title), "MECMP %s###mecmp", app::kVersion);
    ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse);
    sessionSection();
    playersSection();
    lookSection();
    ImGui::Spacing();
    ImGui::TextDisabled("%s closes this menu", config::get().menuKey.c_str());
    ImGui::End();
}

} // namespace mecmp::ui::menu
