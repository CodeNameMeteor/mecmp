#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/menu.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/mod.h"
#include "core/config.h"
#include "game/ghosts.h"
#include "game/models.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "ui/gamepad.h"
#include "ui/overlay.h"

namespace mecmp::ui::menu {
namespace {

char g_name[net::protocol::kMaxNameBytes + 1] = {}; // what fits on the wire
char g_code[64] = {};
bool g_inited = false;
bool g_keyCapture = false; // waiting for a new menu key
DWORD g_hintUntil = 0;

void copyToClipboard(const std::string& text) {
    if (text.empty() || !OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), text.size() + 1);
            GlobalUnlock(h);
            if (SetClipboardData(CF_TEXT, h)) h = nullptr; // the system owns the memory now
        }
        if (h) GlobalFree(h);
    }
    CloseClipboard();
}

std::string fromClipboard() {
    std::string s;
    if (!OpenClipboard(nullptr)) return s;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        // Another program wrote this: read at most the block's size, and stop at the first NUL.
        const SIZE_T size = GlobalSize(h);
        if (const char* p = static_cast<const char*>(GlobalLock(h))) {
            SIZE_T len = 0;
            while (len < size && p[len]) ++len;
            s.assign(p, len);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string enteredName() { return g_name[0] ? std::string(g_name) : std::string("Runner"); }

void startSession(const std::string& join) {
    const auto cfg = config::get();
    const std::string name = enteredName();
    net::start(cfg->port, join, cfg->loopbackOffset, name);
    config::change([name, join](config::Settings& c) { c.name = name; c.join = join; });
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
        ImGui::Text("Joined %s", config::get()->join.c_str());
        ImGui::SameLine();
        if (net::connectedToHost()) ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "connected");
        else ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "waiting for the host...");
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputText("Your name", g_name, sizeof(g_name));
    ImGui::SameLine();
    if (ImGui::Button("Set name")) {
        const std::string name = enteredName();
        net::setName(name);
        config::change([name](config::Settings& c) { c.name = name; });
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
    const bool noCode = g_code[0] == 0;
    ImGui::BeginDisabled(noCode);
    if (ImGui::Button("Join")) startSession(g_code);
    ImGui::EndDisabled();
    if (noCode && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Enter the host's join code first.");
    if (ImGui::Button("Solo test (mirror me)")) startSession("127.0.0.1:" + std::to_string(config::get()->port));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Test on your own PC: a ghost copies your moves 3 m away (it does not turn with you; look around for it).");
}

void playersSection() {
    ImGui::SeparatorText("Players");
    const auto remotes = net::remotes(5000);
    if (remotes.empty()) {
        ImGui::TextDisabled("Nobody else here yet.");
        return;
    }
    if (ImGui::BeginTable("players", 3, ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& r : remotes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", r.name.empty() ? "(joining...)" : r.name.c_str(), r.loopback ? " (you, mirrored)" : "");
            ImGui::TableNextColumn();
            if (r.ageMs < 1000) ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "live");
            else ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%.1f s ago", r.ageMs / 1000.f);
            ImGui::TableNextColumn();
            if (r.loopback || r.ageMs >= 1000) {
                ImGui::TextUnformatted("");
            } else if (r.lossPct < 2.f) {
                ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "good");
            } else {
                const ImVec4 c = r.lossPct < 10.f ? ImVec4(1.f, 0.85f, 0.3f, 1.f) : ImVec4(1.f, 0.4f, 0.35f, 1.f);
                ImGui::TextColored(c, "%s, %.0f%% lost", r.lossPct < 10.f ? "fair" : "poor", r.lossPct);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Share of their updates that did not arrive in the last 5 s.");
        }
        ImGui::EndTable();
    }
}

// Colour swatches for the echo colour names (the game's own colours are shader parameters).
ImVec4 swatch(const std::string& colour) {
    struct C { const char* name; ImVec4 v; };
    static const C kC[] = {{"Red", {0.85f, 0.15f, 0.15f, 1}},  {"Blue", {0.2f, 0.45f, 0.95f, 1}},
                           {"Green", {0.25f, 0.8f, 0.3f, 1}},  {"Orange", {0.95f, 0.55f, 0.1f, 1}},
                           {"Teal", {0.1f, 0.75f, 0.75f, 1}},  {"Yellow", {0.95f, 0.85f, 0.15f, 1}},
                           {"Black", {0.08f, 0.08f, 0.1f, 1}}, {"White", {0.95f, 0.95f, 0.95f, 1}},
                           {"Purple", {0.6f, 0.3f, 0.85f, 1}}, {"Pink", {0.95f, 0.45f, 0.7f, 1}}};
    for (const C& c : kC)
        if (colour == c.name) return c.v;
    return ImVec4(0.6f, 0.6f, 0.65f, 1);
}

void setMyLook(uint32_t hash) {
    net::setLook(hash);
    const std::string look = config::lookName(hash);
    config::change([look](config::Settings& c) { c.look = look; });
}

void myLookSection() {
    ImGui::SeparatorText("Your look");
    const auto cfg = config::get();
    const uint32_t mine = config::lookHash(cfg->look);
    const auto echoes = game::models::echoes();
    if (ImGui::RadioButton("Faith", mine == 0)) setMyLook(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("Ghost", mine != 0)) {
        if (!mine && !echoes.empty()) setMyLook(game::models::defaultEcho());
        else if (!mine) setMyLook(0xffffffffu); // chosen before the list is known: pick once it is
    }
    if (!mine) return;
    if (echoes.empty()) {
        ImGui::TextDisabled("The ghost styles load in free roam.");
        return;
    }
    const game::models::Echo* cur = nullptr;
    for (const auto& e : echoes)
        if (e.hash == mine) cur = &e;
    if (!cur) { // unknown hash (e.g. picked before the list loaded): take the default
        const uint32_t d = game::models::defaultEcho();
        setMyLook(d);
        for (const auto& e : echoes)
            if (e.hash == d) cur = &e;
        if (!cur) return;
    }
    // Style, then the colours that style comes in.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("Style", cur->style.c_str())) {
        std::string last;
        for (const auto& e : echoes) {
            if (e.style == last) continue;
            last = e.style;
            if (ImGui::Selectable(e.style.c_str(), e.style == cur->style)) {
                // keep the colour if the new style has it
                uint32_t pick = e.hash;
                for (const auto& f : echoes)
                    if (f.style == e.style && f.colour == cur->colour) pick = f.hash;
                setMyLook(pick);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    for (const auto& e : echoes) {
        if (e.style != cur->style) continue;
        ImGui::PushID(static_cast<int>(e.hash));
        const ImVec4 c = swatch(e.colour);
        if (e.hash == cur->hash) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f);
        if (ImGui::ColorButton(e.colour.c_str(), c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                               ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
            setMyLook(e.hash);
        if (e.hash == cur->hash) ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.colour.c_str());
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

void lookSection() {
    ImGui::SeparatorText("Other players");
    bool showNames = config::get()->showNames;
    if (ImGui::Checkbox("Names above players", &showNames))
        config::change([showNames](config::Settings& c) { c.showNames = showNames; });
    bool showNotices = config::get()->showNotices;
    if (ImGui::Checkbox("Join/leave notices", &showNotices))
        config::change([showNotices](config::Settings& c) { c.showNotices = showNotices; });
    if (ImGui::Button("Show ghosts")) game::ghosts::requestSpawnOrRefresh();
    ImGui::SameLine();
    if (ImGui::Button("Hide ghosts")) game::ghosts::requestHideAll();
}

void controlsSection() {
    ImGui::SeparatorText("Controls");
    const auto cfg = config::get();
    // Keyboard key that opens the menu.
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Menu key:");
    ImGui::SameLine();
    if (g_keyCapture) {
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "press a key (Esc cancels)");
        int count = 0;
        const ui::MenuKey* keys = ui::menuKeys(count);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_keyCapture = false;
        } else {
            for (int i = 0; i < count; ++i) {
                if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(keys[i].imguiKey), false)) continue;
                const std::string name = keys[i].name;
                ui::setMenuKey(name);
                config::change([name](config::Settings& c) { c.menuKey = name; });
                g_keyCapture = false;
                break;
            }
        }
        if (!g_keyCapture) ui::setKeyCapture(false);
    } else {
        ImGui::Text("%s", cfg->menuKey.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Change##key")) {
            g_keyCapture = true;
            ui::setKeyCapture(true);
        }
    }
    // Gamepad buttons that open it.
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Gamepad:");
    ImGui::SameLine();
    uint16_t got = 0;
    if (gamepad::capturing()) {
        if (gamepad::captureDone(got)) {
            const std::string combo = gamepad::format(got);
            gamepad::setBinding(got);
            config::change([combo](config::Settings& c) { c.menuPad = combo; });
        } else {
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "hold the buttons together, then let go");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##pad")) gamepad::cancelCapture();
        }
    } else {
        const uint16_t bits = gamepad::parse(cfg->menuPad);
        ImGui::Text("%s", bits ? gamepad::format(bits).c_str() : "none");
        ImGui::SameLine();
        if (ImGui::SmallButton("Change##pad")) gamepad::beginCapture();
        if (bits) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##pad")) {
                gamepad::setBinding(0);
                config::change([](config::Settings& c) { c.menuPad.clear(); });
            }
        }
        if (!gamepad::active()) ImGui::TextDisabled("No gamepad seen yet.");
    }
}

// "INSERT or LS+RS"
std::string openKeys(const config::Settings& cfg) {
    const uint16_t pad = gamepad::parse(cfg.menuPad);
    return pad ? cfg.menuKey + " or " + gamepad::format(pad) : cfg.menuKey;
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
        snprintf(g_name, sizeof(g_name), "%s", config::get()->name.c_str());
        snprintf(g_code, sizeof(g_code), "%s", config::get()->join.c_str());
        g_hintUntil = GetTickCount() + 10000;
    }
    if (!open) {
        // A short hint after start-up, top-left.
        if (GetTickCount() < g_hintUntil) {
            ImGui::SetNextWindowPos(ImVec2(16, 16));
            ImGui::SetNextWindowBgAlpha(0.6f);
            ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
            ImGui::Text("MECMP %s: press %s for the multiplayer menu", app::kVersion, openKeys(*config::get()).c_str());
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
    myLookSection();
    lookSection();
    controlsSection();
    ImGui::Spacing();
    ImGui::TextDisabled("%s closes this menu", openKeys(*config::get()).c_str());
    ImGui::End();
}

} // namespace mecmp::ui::menu
