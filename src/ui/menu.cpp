#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/menu.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/mod.h"
#include "core/config.h"
#include "game/mumble_link.h"
#include "game/ghosts.h"
#include "game/ping_point.h"
#include "game/raycast.h"
#include "game/teleport.h"
#include "game/models.h"
#include "imgui.h"
#include "net/net.h"
#include "net/protocol.h"
#include "net/serverlist.h"
#include "ui/gamepad.h"
#include "ui/glyph.h"
#include "ui/overlay.h"

namespace mecmp::ui::menu {
namespace {

char g_name[net::protocol::kMaxNameBytes + 1] = {}; // what fits on the wire
char g_code[64] = {};
char g_label[config::kWheels][config::kPingSlots][net::protocol::kMaxPingLabelBytes + 1] = {}; // the marker wheels' titles
char g_wheelName[config::kWheels][net::protocol::kMaxPingLabelBytes + 1] = {};                  // the wheels' own names
char g_reactLabel[config::kPingSlots][net::protocol::kMaxPingLabelBytes + 1] = {};              // the quick answers
bool g_inited = false;
bool g_keyCapture = false; // waiting for a new menu key
int g_bindCapture = -1;        // which key of the Controls tab is being recorded (see kKeyBinds; -1 = none)
std::string g_bindNote[8];     // why the last key was refused (empty = nothing to say)
gamepad::Action g_padCapture = gamepad::Action::Count; // which gamepad binding is being recorded (Count = none)
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
        const std::string server = net::serverName();
        if (!server.empty()) ImGui::Text("Server: %s (%s)", server.c_str(), config::get()->join.c_str());
        else ImGui::Text("Joined %s", config::get()->join.c_str());
        ImGui::SameLine();
        if (net::connectedToHost()) ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "connected");
        else ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "waiting for the host...");
    }
    if (running) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Leave")) net::leave();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop this session and go offline");
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputText("Your name", g_name, sizeof(g_name));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The name other players see above your ghost");
    ImGui::SameLine();
    if (ImGui::Button("Set name")) {
        const std::string name = enteredName();
        net::setName(name);
        config::change([name](config::Settings& c) { c.name = name; });
    }

    bool autoStart = config::get()->autoStart;
    if (ImGui::Checkbox("Start this session when the game starts", &autoStart))
        config::change([autoStart](config::Settings& c) { c.autoStart = autoStart; });
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the mod stays offline until you host or join here.\n"
                          "On: it hosts, or joins the last session you joined, at launch.");

    ImGui::Spacing();
    if (ImGui::Button("Host a session")) startSession("");
    ImGui::SameLine();
    ImGui::TextDisabled("(friends join with your code)");

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputTextWithHint("##code", "join code or server address", g_code, sizeof(g_code));
    ImGui::SameLine();
    if (ImGui::Button("Paste")) snprintf(g_code, sizeof(g_code), "%s", fromClipboard().c_str());
    ImGui::SameLine();
    const bool noCode = g_code[0] == 0;
    ImGui::BeginDisabled(noCode);
    if (ImGui::Button("Join")) startSession(g_code);
    ImGui::EndDisabled();
    if (noCode && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Enter the host's join code, or a server's address (name or ip, :port if not 47800).");
    if (ImGui::Button("Solo test (mirror me)")) startSession("127.0.0.1:" + std::to_string(config::get()->port));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Test on your own PC: a ghost copies your moves 3 m away (it does not turn with you; look around for it).");
}

// Saved dedicated servers: name, players and round trip of each (asked while the menu is open).
char g_addServer[128] = {};
void serversSection() {
    ImGui::SeparatorText("Servers");
    const auto cfg = config::get();
    std::vector<std::string> saved = net::servers::split(cfg->servers);
    net::servers::update(saved, true);
    const auto entries = net::servers::snapshot();
    std::string remove;
    if (entries.empty()) ImGui::TextDisabled("No servers saved yet: add one by its address below.");
    if (!entries.empty() &&
        ImGui::BeginTable("##servers", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool joined = net::running() && !net::hosting() && cfg->join == e.address;
            const bool otherVersion = e.answered && e.protocol != net::protocol::kProtocol;
            ImGui::Text("%s%s", e.answered && !e.name.empty() ? e.name.c_str() : e.address.c_str(), joined ? "  (joined)" : "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.address.c_str());
            ImGui::TableNextColumn();
            if (e.answered) ImGui::Text("%d/%d", e.players, e.maxPlayers);
            else ImGui::TextDisabled("%s", e.resolved ? "no answer" : "...");
            ImGui::TableNextColumn();
            if (otherVersion) ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "other version");
            else if (e.answered) ImGui::Text("%d ms", e.pingMs);
            if (otherVersion && ImGui::IsItemHovered())
                ImGui::SetTooltip("The server runs MECMP protocol %u, this mod %u: update one of them.", e.protocol,
                                  net::protocol::kProtocol);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(joined || otherVersion);
            if (ImGui::SmallButton("Join")) {
                snprintf(g_code, sizeof(g_code), "%s", e.address.c_str());
                startSession(e.address);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) remove = e.address;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from the list");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##addserver", "server address", g_addServer, sizeof(g_addServer));
    ImGui::SameLine();
    std::string add = g_addServer;
    while (!add.empty() && add.back() == ' ') add.pop_back();
    while (!add.empty() && add.front() == ' ') add.erase(add.begin());
    const bool full = saved.size() >= config::kMaxServers;
    ImGui::BeginDisabled(add.empty() || full);
    if (ImGui::Button("Add")) {
        bool have = false;
        for (const auto& s : saved) have = have || s == add;
        if (!have) saved.push_back(add);
        g_addServer[0] = 0;
        const std::string list = net::servers::join(saved);
        config::change([list](config::Settings& c) { c.servers = list; });
        net::servers::refreshNow();
    }
    ImGui::EndDisabled();
    if (full && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The list is full (%d servers): remove one first", static_cast<int>(config::kMaxServers));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) net::servers::refreshNow();
    if (!remove.empty()) {
        std::vector<std::string> kept;
        for (const auto& s : saved)
            if (s != remove) kept.push_back(s);
        const std::string list = net::servers::join(kept);
        config::change([list](config::Settings& c) { c.servers = list; });
    }
}

void playersSection() {
    ImGui::SeparatorText("Players");
    const auto remotes = net::remotes(5000);
    if (remotes.empty()) {
        ImGui::TextDisabled("Nobody else here yet.");
        return;
    }
    if (ImGui::BeginTable("players", 4, ImGuiTableFlags_SizingStretchProp)) {
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
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(r.id));
            const bool can = game::teleport::available() && !game::teleport::busy() && r.ageMs < 1000;
            ImGui::BeginDisabled(!can);
            if (ImGui::SmallButton("Go to")) game::teleport::requestGoTo(r.id);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(game::teleport::available() ? "Travel to this player" : "Not available in this game build");
            ImGui::PopID();
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

// The look goes to the other players and, if asked for, onto the player's own character.
void publishMyLook(uint32_t hash, int outfit, bool self) {
    net::setLook(hash, static_cast<uint8_t>(outfit));
    game::models::setOwnLook(hash, static_cast<uint8_t>(outfit), self);
}

void setMyLook(uint32_t hash) {
    const auto cfg = config::get();
    publishMyLook(hash, cfg->outfit, cfg->lookSelf);
    const std::string look = config::lookName(hash);
    config::change([look](config::Settings& c) { c.look = look; });
}

// Under "Your look": Faith's outfit, and whether the look is yours in your own game as well.
void myLookRows(const config::Settings& cfg, uint32_t mine) {
    namespace wear = net::protocol;
    const int worn = cfg.outfit;
    if (!mine) { // Faith's outfit; a ghost has none
        const bool self = cfg.lookSelf;
        auto set = [self](int now) {
            publishMyLook(0, now, self);
            config::change([now](config::Settings& c) { c.outfit = now; });
        };
        {
            const bool prologue = (worn & wear::kWearPrologue) != 0;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
            if (ImGui::BeginCombo("Outfit", prologue ? "Prologue" : "As in your game")) {
                // The prologue outfit as the game shows it: with the GridLink, without the MagRope and the
                // tool bag. Each can be changed below.
                const int kit = wear::kWearGridLink | wear::kWearNoMagRope | wear::kWearNoToolbag;
                if (ImGui::Selectable("As in your game", !prologue)) set(worn & ~(wear::kWearPrologue | kit));
                if (ImGui::Selectable("Prologue", prologue)) set(worn | wear::kWearPrologue | kit);
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("\"As in your game\": each player sees you in the outfit of their own story progress.");
            if (!prologue) { // the prologue outfit has sleeves
                const int tattoo = worn & wear::kWearTattoo ? 1 : worn & wear::kWearNoTattoo ? 2 : 0;
                static const char* const kTattoo[] = {"As in your game", "Tattoo", "No tattoo"};
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
                if (ImGui::BeginCombo("Arms", kTattoo[tattoo])) {
                    for (int i = 0; i < 3; ++i)
                        if (ImGui::Selectable(kTattoo[i], i == tattoo))
                            set((worn & ~(wear::kWearTattoo | wear::kWearNoTattoo)) | (i == 1 ? wear::kWearTattoo : i == 2 ? wear::kWearNoTattoo : 0));
                    ImGui::EndCombo();
                }
            }
        }
        bool magRope = !(worn & wear::kWearNoMagRope), toolbag = !(worn & wear::kWearNoToolbag);
        if (ImGui::Checkbox("MagRope", &magRope)) set(magRope ? worn & ~wear::kWearNoMagRope : worn | wear::kWearNoMagRope);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: without the MagRope on your arm (you can still use it).\nOn: the one you have in your game, if you have it yet.");
        ImGui::SameLine();
        if (ImGui::Checkbox("Tool bag", &toolbag)) set(toolbag ? worn & ~wear::kWearNoToolbag : worn | wear::kWearNoToolbag);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: without the tool bag on your back.\nOn: the one you have in your game, if you have it yet.");
        ImGui::SameLine();
        bool gridLink = (worn & wear::kWearGridLink) != 0;
        if (ImGui::Checkbox("GridLink", &gridLink)) set(gridLink ? worn | wear::kWearGridLink : worn & ~wear::kWearGridLink);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: with the GridLink on your hand, even where your story has none");
    }
    bool self = cfg.lookSelf;
    if (ImGui::Checkbox("Look like this in my own game too", &self)) {
        publishMyLook(mine, cfg.outfit, self);
        config::change([self](config::Settings& c) { c.lookSelf = self; });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: your own body (arms, legs, shadow) changes as well.\nOff: only the other players see the look.");
}

void myLookSection() {
    ImGui::SeparatorText("Your look");
    const auto cfg = config::get();
    const uint32_t mine = config::lookHash(cfg->look);
    const auto echoesList = game::models::echoes();
    const auto& echoes = *echoesList;
    if (ImGui::RadioButton("Faith", mine == 0)) setMyLook(0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the other players see you");
    ImGui::SameLine();
    if (ImGui::RadioButton("Ghost", mine != 0)) {
        if (!mine && !echoes.empty()) setMyLook(game::models::defaultEcho());
        else if (!mine) setMyLook(0xffffffffu); // chosen before the list is known: pick once it is
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the other players see you");
    myLookRows(*cfg, mine);
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

// One line under the Mumble checkbox: what is missing, or that positions go out.
void voiceStatus() {
    const game::mumble::Status s = game::mumble::status();
    if (!s.enabled) return;
    const ImVec4 warn(1.f, 0.75f, 0.3f, 1.f), ok(0.5f, 0.9f, 0.5f, 1.f);
    ImGui::Indent();
    if (!s.mumbleFound) {
        ImGui::TextColored(warn, "Mumble is not running");
    } else if (!s.sending) {
        ImGui::TextColored(warn, "Mumble found; positions start once you are in a level");
    } else if (!s.inSession) {
        ImGui::TextColored(warn, "Sending to Mumble; host or join a session to hear the others");
    } else {
        ImGui::TextColored(ok, "Sending to Mumble (%s)", s.context);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In Mumble: Settings > Plugins: enable Link and \"Link to Game and Transmit Position\";\n"
                          "Settings > Audio Output: enable Positional Audio. When Mumble picks the game up its\n"
                          "log shows a \"linked\" line. Everyone must be on the same Mumble server; players in\n"
                          "the same MECMP session hear each other from where they are.");
    ImGui::Unindent();
}

void lookSection() {
    ImGui::SeparatorText("Other players");
    bool showNames = config::get()->showNames;
    if (ImGui::Checkbox("Names above players", &showNames))
        config::change([showNames](config::Settings& c) { c.showNames = showNames; });
    bool showNotices = config::get()->showNotices;
    if (ImGui::Checkbox("Notices (join/leave, markers)", &showNotices))
        config::change([showNotices](config::Settings& c) { c.showNotices = showNotices; });
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Short messages at the top of the screen when someone joins, leaves or places a marker");
    bool showChat = config::get()->showChat;
    if (ImGui::Checkbox("Chat", &showChat)) config::change([showChat](config::Settings& c) { c.showChat = showChat; });
    ImGui::SameLine();
    ImGui::TextDisabled("(%s to type)", config::get()->chatKey.c_str());
    bool mumble = config::get()->mumbleLink;
    if (ImGui::Checkbox("Positional voice (Mumble)", &mumble))
        config::change([mumble](config::Settings& c) { c.mumbleLink = mumble; });
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("With Mumble running (Positional Audio and the Link plugin on, everyone on the same\n"
                          "Mumble server), players in this session hear each other from where they are.");
    voiceStatus();
    if (ImGui::Button("Show ghosts")) game::ghosts::requestSpawnOrRefresh();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brings the other players' ghosts back if they are missing");
    ImGui::SameLine();
    if (ImGui::Button("Hide ghosts")) game::ghosts::requestHideAll();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hides the other players' ghosts until you show them again");
}

// The action key pressed this frame (as ping_key names it: a letter, ENTER or a menu key), if any.
bool pressedActionKey(std::string& name) {
    for (int i = 0; i < 26; ++i) {
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + i), false)) continue;
        name.assign(1, static_cast<char>('A' + i));
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        name = "ENTER";
        return true;
    }
    int count = 0;
    const ui::MenuKey* keys = ui::menuKeys(count);
    for (int i = 0; i < count; ++i) {
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(keys[i].imguiKey), false)) continue;
        name = keys[i].name;
        return true;
    }
    return false;
}

// One gamepad binding: its buttons, Change (hold the buttons, let go) and Clear.
void padRow(const char* label, gamepad::Action action, const std::string& current, std::string config::Settings::*field,
            const char* tip = nullptr) {
    ImGui::PushID("pad"); // key rows and gamepad rows share a section: keep their buttons' ids apart
    ImGui::PushID(static_cast<int>(action));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    uint16_t got = 0;
    if (gamepad::capturing() && g_padCapture == action) {
        if (gamepad::captureDone(got)) {
            const std::string combo = gamepad::format(got);
            gamepad::setBinding(got, action);
            config::change([combo, field](config::Settings& c) { c.*field = combo; });
            g_padCapture = gamepad::Action::Count;
        } else {
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "hold the buttons together, then let go");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) {
                gamepad::cancelCapture();
                g_padCapture = gamepad::Action::Count;
            }
        }
    } else {
        const uint16_t bits = gamepad::parse(current);
        ImGui::Text("%s", bits ? gamepad::labelCombo(gamepad::format(bits)).c_str() : "none");
        ImGui::SameLine();
        // One binding is recorded at a time, and only a gamepad can press its buttons.
        ImGui::BeginDisabled(gamepad::capturing() || !gamepad::active());
        if (ImGui::SmallButton("Change")) {
            g_padCapture = action;
            gamepad::beginCapture();
        }
        ImGui::EndDisabled();
        if (bits) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                gamepad::setBinding(0, action);
                config::change([field](config::Settings& c) { (c.*field).clear(); });
            }
        }
        if (!gamepad::active()) {
            ImGui::SameLine();
            ImGui::TextDisabled("no gamepad connected");
        }
    }
    ImGui::PopID();
    ImGui::PopID();
}

// --- Markers --------------------------------------------------------------------------------------

// A slider that is saved when it is let go, not on every step of the drag. `value` is what it shows
// from the first change until the setting has caught up.
struct SliderEdit { int value = 0; bool editing = false; };
SliderEdit g_lifeEdit, g_maxEdit;
struct FloatEdit { float value = 1.f; bool editing = false; };
FloatEdit g_sensEdit;

// True once, when the slider was let go after a change: the caller saves `edit.value`.
bool settingSlider(const char* label, SliderEdit& edit, int current, int lo, int hi, const char* format, ImGuiSliderFlags flags) {
    int v = edit.editing ? edit.value : current;
    if (ImGui::SliderInt(label, &v, lo, hi, format, flags | ImGuiSliderFlags_AlwaysClamp)) {
        edit.value = v;
        edit.editing = true;
    }
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    if (edit.editing && !done && !ImGui::IsItemActive() && current == edit.value) edit.editing = false;
    return done;
}

// One of the two marker colors: a swatch that opens a picker.
void markerColor(const char* id, const std::string& current, std::string config::Settings::*field, const char* tip, uint8_t fallback) {
    uint8_t rgb[3] = {fallback, fallback, fallback};
    net::protocol::parseRgb(current, rgb);
    float col[3] = {rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f};
    if (ImGui::ColorEdit3(id, col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoTooltip)) {
        for (int i = 0; i < 3; ++i) rgb[i] = static_cast<uint8_t>(col[i] < 0 ? 0 : (col[i] > 1 ? 255 : col[i] * 255.f + 0.5f));
        const std::string hex = net::protocol::formatRgb(rgb);
        config::change([hex, field](config::Settings& c) { c.*field = hex; });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
}

// "Wheel 1: Nice!"
std::string slotName(const config::Settings& cfg, int wheel, int slot) {
    const std::string& label = cfg.wheels[wheel][slot].label;
    return "Wheel " + std::to_string(wheel + 1) + ": " + (label.empty() ? "slot " + std::to_string(slot + 1) : label);
}

// The keys of the Markers section: a letter, Enter or one of the menu keys (and for some a mouse
// button). Change = press the new one; Clear = none.
struct KeyBind {
    const char* tip;
    std::string config::Settings::*field;
    bool (*set)(const std::string&);
    int mouse;  // 0 = the keyboard only, 1 = also Mouse 4 / 5, 2 = also the middle button
};
const KeyBind kKeyBinds[8] = {
    {"Tap to place a marker. Hold to open the marker wheel.", &config::Settings::pingKey, &ui::setPingKey, 1},
    {"Tap twice to remove the marker you look at (or stand at). Hold for a moment to remove all your markers.",
     &config::Settings::pingClearKey, &ui::setPingClearKey, 2},
    {"Opens the chat line (Enter sends, Escape cancels).", &config::Settings::chatKey, &ui::setChatKey, 0},
    {"Look at another player's marker: tap sends your last quick answer to them, hold opens the answers wheel.",
     &config::Settings::reactKey, &ui::setReactKey, 1},
    {"Places a marker the other way: where you stand if your markers go where you look, and where you look if they go "
     "where you stand.",
     &config::Settings::altKey, &ui::setAltKey, 1},
    {"Hides names, markers, arrows, notices and chat lines (for a screenshot or a recording). Press again to show them.",
     &config::Settings::hideKey, &ui::setHideKey, 1},
    {"While the marker wheel is open: the previous wheel. The Left arrow and the mouse wheel do the same.",
     &config::Settings::wheelPrevKey, &ui::setWheelPrevKey, 1},
    {"While the marker wheel is open: the next wheel. The Right arrow and the mouse wheel do the same.",
     &config::Settings::wheelNextKey, &ui::setWheelNextKey, 1},
};

void keyRow(const config::Settings& cfg, int which) {
    const KeyBind& bind = kKeyBinds[which];
    const auto field = bind.field;
    const std::string& current = cfg.*field;
    ImGui::PushID("key");
    ImGui::PushID(which);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Key:");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bind.tip);
    ImGui::SameLine();
    if (g_bindCapture == which) {
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), bind.mouse ? "press a key or a mouse button (Esc cancels)" : "press a key (Esc cancels)");
        std::string name;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_bindCapture = -1;
        } else {
            bool got = pressedActionKey(name);
            for (int m = bind.mouse >= 2 ? 2 : 3; m <= 4 && !got && bind.mouse; ++m)
                if (ImGui::IsMouseClicked(m)) {
                    name = "MOUSE" + std::to_string(m + 1);
                    got = true;
                }
            if (got) {
                if (bind.set(name)) {
                    config::change([name, field](config::Settings& c) { c.*field = name; });
                    g_bindNote[which].clear();
                } else {
                    g_bindNote[which] = ui::keyLabel(name) + " is already in use";
                }
                g_bindCapture = -1;
            }
        }
        if (g_bindCapture < 0) ui::setKeyCapture(false);
    } else {
        ImGui::Text("%s", current.empty() ? "none" : ui::keyLabel(current).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(g_keyCapture || g_bindCapture >= 0 || gamepad::capturing());
        if (ImGui::SmallButton("Change")) {
            g_bindCapture = which;
            g_bindNote[which].clear();
            ui::setKeyCapture(true);
        }
        ImGui::EndDisabled();
        if (!current.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                bind.set("");
                config::change([field](config::Settings& c) { (c.*field).clear(); });
            }
        }
        if (!g_bindNote[which].empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", g_bindNote[which].c_str());
        }
    }
    ImGui::PopID();
    ImGui::PopID();
}

// What a short press of the marker key places.
void tapModeRows(const config::Settings& cfg) {
    ImGui::Text("Pressing %s:", cfg.pingKey.empty() ? "the marker key" : cfg.pingKey.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What a quick tap of the marker key places");
    if (ImGui::RadioButton("places the last marker you picked", cfg.pingTap == 0))
        config::change([](config::Settings& c) { c.pingTap = 0; });
    if (ImGui::RadioButton("always places:", cfg.pingTap != 0)) config::change([](config::Settings& c) { c.pingTap = 1; });
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13);
    const int fixedWheel = cfg.pingFixedWheel < cfg.wheelCount ? cfg.pingFixedWheel : 0;
    if (ImGui::BeginCombo("##pingtap", slotName(cfg, fixedWheel, cfg.pingFixedSlot).c_str())) {
        for (int w = 0; w < cfg.wheelCount; ++w)
            for (int i = 0; i < cfg.wheelSlots[w]; ++i) {
                const std::string id = slotName(cfg, w, i) + "##" + std::to_string(w * config::kPingSlots + i);
                if (ImGui::Selectable(id.c_str(), w == fixedWheel && i == cfg.pingFixedSlot))
                    config::change([w, i](config::Settings& c) {
                        c.pingTap = 1;
                        c.pingFixedWheel = w;
                        c.pingFixedSlot = i;
                    });
            }
        ImGui::EndCombo();
    }
}

// A square button showing icon `glyph`. True when clicked.
bool iconButton(const char* id, uint8_t glyph, float size, ImU32 fill, ImU32 accent, bool selected) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(selected ? ImGuiCol_Header : ImGuiCol_FrameBg));
    const bool clicked = ImGui::Button(id, ImVec2(size, size));
    ImGui::PopStyleColor();
    const ImVec2 a = ImGui::GetItemRectMin();
    drawGlyph(ImGui::GetWindowDrawList(), glyph, ImVec2(a.x + size * 0.5f, a.y + size * 0.5f), size * 0.33f, fill, 1.f, accent);
    return clicked;
}

void loadLabels(int w, const config::Wheel& wheel) {
    for (int i = 0; i < config::kPingSlots; ++i) snprintf(g_label[w][i], sizeof(g_label[w][i]), "%s", wheel[i].label.c_str());
}

// Removes wheel `w` (one the player added): the wheels after it move down one place.
void removeWheel(const config::Settings& cfg, int w) {
    for (int k = w; k + 1 < cfg.wheelCount; ++k) loadLabels(k, cfg.wheels[k + 1]);
    loadLabels(cfg.wheelCount - 1, config::defaultWheel(cfg.wheelCount - 1));
    for (int k = w; k + 1 < cfg.wheelCount; ++k) memcpy(g_wheelName[k], g_wheelName[k + 1], sizeof(g_wheelName[k]));
    g_wheelName[cfg.wheelCount - 1][0] = 0;
    config::change([w](config::Settings& c) {
        if (w < config::kDefaultWheels || w >= c.wheelCount) return;
        for (int k = w; k + 1 < c.wheelCount; ++k) {
            c.wheels[k] = c.wheels[k + 1];
            c.wheelSlots[k] = c.wheelSlots[k + 1];
            c.wheelNames[k] = c.wheelNames[k + 1];
        }
        c.wheelSlots[c.wheelCount - 1] = config::kPingSlots;
        c.wheelNames[c.wheelCount - 1].clear();
        --c.wheelCount;
        c.wheels[c.wheelCount] = config::defaultWheel(c.wheelCount);
        // What pointed at the removed wheel goes back to the first marker; what came after it follows its wheel.
        auto fix = [w](int& wheel, int* slot) {
            if (wheel == w) {
                wheel = 0;
                if (slot) *slot = 0;
            } else if (wheel > w) {
                --wheel;
            }
        };
        fix(c.pingWheel, nullptr);
        fix(c.pingLastWheel, &c.pingLastSlot);
        fix(c.pingFixedWheel, &c.pingFixedSlot);
    });
}

// One slot per row: the icon (click for the list of icons) and the title, which is what the others
// read. Rows past `shown` are kept but not on the wheel: greyed out. `setGlyph` / `setLabel` store a change.
template <class SetGlyph, class SetLabel>
void slotRows(const config::Settings& cfg, const config::Wheel& slots, int shown, char (*buf)[net::protocol::kMaxPingLabelBytes + 1],
              SetGlyph setGlyph, SetLabel setLabel) {
    uint8_t rgb[3] = {255, 210, 60}, acc[3] = {255, 255, 255};
    net::protocol::parseRgb(cfg.pingColor, rgb);
    net::protocol::parseRgb(cfg.pingAccent, acc);
    const ImU32 fill = IM_COL32(rgb[0], rgb[1], rgb[2], 255), accent = IM_COL32(acc[0], acc[1], acc[2], 255);
    const float h = ImGui::GetFrameHeight();
    for (int i = 0; i < config::kPingSlots; ++i) {
        ImGui::PushID(i);
        ImGui::BeginDisabled(i >= shown);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d", i + 1);
        ImGui::SameLine();
        if (iconButton("##icon", slots[i].glyph, h, fill, accent, false)) ImGui::OpenPopup("icons");
        if (ImGui::BeginPopup("icons")) {
            for (int g = 0; g < kGlyphCount; ++g) {
                ImGui::PushID(g);
                if (g % 8) ImGui::SameLine();
                if (iconButton("##pick", static_cast<uint8_t>(g), h * 1.4f, fill, accent, g == slots[i].glyph)) {
                    setGlyph(i, static_cast<uint8_t>(g));
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::InputTextWithHint("##label", "(no title)", buf[i], sizeof(buf[i])))
            setLabel(i, net::protocol::cleanPingLabel(buf[i]));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The text others see. Up to %d bytes (about %d Latin or %d Cyrillic\n"
                              "letters). Leave empty for an icon only.%s",
                              static_cast<int>(net::protocol::kMaxPingLabelBytes), static_cast<int>(net::protocol::kMaxPingLabelBytes),
                              static_cast<int>(net::protocol::kMaxPingLabelBytes / 2),
                              i >= shown ? "\nNot on the wheel at the moment (see Slots); kept for later." : "");
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

SliderEdit g_slotsEdit[config::kWheels + 1]; // [kWheels] = the quick answers

// One wheel: its name, how many slots it shows, and the slots.
void wheelEditor(const config::Settings& cfg, int w) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::InputTextWithHint("Name", ("Wheel " + std::to_string(w + 1)).c_str(), g_wheelName[w], sizeof(g_wheelName[w]))) {
        const std::string text = net::protocol::cleanPingLabel(g_wheelName[w]);
        config::change([w, text](config::Settings& c) { c.wheelNames[w] = text; });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The title shown over this wheel when it is open. Empty: \"Wheel %d\".", w + 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (settingSlider("Slots", g_slotsEdit[w], cfg.wheelSlots[w], config::kMinSlots, config::kPingSlots, "%d", 0)) {
        const int n = g_slotsEdit[w].value;
        config::change([w, n](config::Settings& c) {
            c.wheelSlots[w] = n;
            if (c.pingLastWheel == w && c.pingLastSlot >= n) c.pingLastSlot = 0;
            if (c.pingFixedWheel == w && c.pingFixedSlot >= n) c.pingFixedSlot = 0;
        });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How many slots this wheel shows (2-9). The others are kept, so raising it again brings them back.");
    const int shown = g_slotsEdit[w].editing ? g_slotsEdit[w].value : cfg.wheelSlots[w];
    slotRows(
        cfg, cfg.wheels[w], shown, g_label[w],
        [w](int i, uint8_t g) { config::change([w, i, g](config::Settings& c) { c.wheels[w][i].glyph = g; }); },
        [w](int i, const std::string& text) { config::change([w, i, text](config::Settings& c) { c.wheels[w][i].label = text; }); });
    const bool own = w >= config::kDefaultWheels; // a wheel the player added
    if (ImGui::SmallButton("Reset this wheel")) {
        loadLabels(w, config::defaultWheel(w));
        config::change([w](config::Settings& c) { c.wheels[w] = config::defaultWheel(w); });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(own ? "Clears every slot of this wheel" : "Back to the default icons and titles");
    if (own) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove this wheel")) removeWheel(cfg, w);
    }
}

// The quick answers: what the react key sends to the owner of the marker you look at.
void reactionEditor(const config::Settings& cfg) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (settingSlider("Slots##react", g_slotsEdit[config::kWheels], cfg.reactionSlots, config::kMinSlots, config::kPingSlots, "%d", 0)) {
        const int n = g_slotsEdit[config::kWheels].value;
        config::change([n](config::Settings& c) {
            c.reactionSlots = n;
            if (c.reactLast >= n) c.reactLast = 0;
        });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many answers the wheel shows (2-9). The others are kept.");
    const int shown = g_slotsEdit[config::kWheels].editing ? g_slotsEdit[config::kWheels].value : cfg.reactionSlots;
    slotRows(
        cfg, cfg.reactions, shown, g_reactLabel,
        [](int i, uint8_t g) { config::change([i, g](config::Settings& c) { c.reactions[i].glyph = g; }); },
        [](int i, const std::string& text) { config::change([i, text](config::Settings& c) { c.reactions[i].label = text; }); });
    if (ImGui::SmallButton("Reset the answers")) {
        const config::Wheel d = config::defaultReactions();
        for (int i = 0; i < config::kPingSlots; ++i) snprintf(g_reactLabel[i], sizeof(g_reactLabel[i]), "%s", d[i].label.c_str());
        config::change([](config::Settings& c) {
            c.reactions = config::defaultReactions();
            c.reactionSlots = config::kDefaultReactionSlots;
            c.reactLast = 0;
        });
    }
}

// --- The lists at the top of the Markers section: my markers, my bookmarks, the others' markers ----

// The title being edited in one of the lists (one at a time).
enum class Renaming { None, Placed, Bookmark };
Renaming g_renaming = Renaming::None;
int g_renameIndex = -1;
char g_renameText[net::protocol::kMaxPingLabelBytes + 1] = {};
bool g_renameFocus = false;

void beginRename(Renaming what, int index, const std::string& current) {
    g_renaming = what;
    g_renameIndex = index;
    snprintf(g_renameText, sizeof(g_renameText), "%s", current.c_str());
    g_renameFocus = true;
}

// The edit line of the title being renamed. True, with the new title in `out`, when it was confirmed.
bool renameField(std::string& out) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
    if (g_renameFocus) {
        ImGui::SetKeyboardFocusHere();
        g_renameFocus = false;
    }
    bool ok = ImGui::InputTextWithHint("##rename", "(no title)", g_renameText, sizeof(g_renameText), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ok = ImGui::SmallButton("OK") || ok;
    ImGui::SameLine();
    const bool cancel = ImGui::SmallButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok) out = net::protocol::cleanPingLabel(g_renameText);
    if (ok || cancel) g_renaming = Renaming::None;
    return ok;
}

// An icon at the start of a list row, as tall as the row.
void rowIcon(uint8_t glyph, const uint8_t rgb[3], const uint8_t accent[3]) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    drawGlyph(ImGui::GetWindowDrawList(), glyph, ImVec2(a.x + h * 0.5f, a.y + h * 0.5f), h * 0.33f, IM_COL32(rgb[0], rgb[1], rgb[2], 255),
              1.f, IM_COL32(accent[0], accent[1], accent[2], 255));
    ImGui::Dummy(ImVec2(h, h));
    ImGui::SameLine();
}

// "42 m, 0:25 left": how far a marker is from me and how long it still stays (only the time while I am
// not in a level).
std::string distanceText(const float pos[3], const float* me);
std::string whereAndLeft(const net::PingInfo& p, const float* me) {
    const std::string away = distanceText(p.pos, me);
    const unsigned left = (p.leftMs + 999) / 1000;
    char b[48];
    snprintf(b, sizeof(b), "%s%s%u:%02u left", away.c_str(), away.empty() ? "" : ", ", left / 60, left % 60);
    return b;
}

// "42 m" from me to `pos`; empty while I am not in a level.
std::string distanceText(const float pos[3], const float* me) {
    if (!me) return std::string();
    float d2 = 0.f;
    for (int i = 0; i < 3; ++i) d2 += (pos[i] - me[i]) * (pos[i] - me[i]);
    char b[24];
    snprintf(b, sizeof(b), "%.0f m", std::sqrt(d2));
    return b;
}

const char* titleOr(const std::string& label) { return label.empty() ? "(no title)" : label.c_str(); }

void addBookmark(const std::string& label, uint8_t glyph, const float pos[3]) {
    config::Bookmark b;
    b.label = label;
    b.glyph = glyph;
    memcpy(b.pos, pos, sizeof(b.pos));
    config::change([b](config::Settings& c) {
        if (c.bookmarks.size() < config::kMaxBookmarks) c.bookmarks.push_back(b);
    });
}

// "Bookmark" for a marker of mine or of someone else.
void bookmarkButton(const config::Settings& cfg, const net::PingInfo& p) {
    const bool full = cfg.bookmarks.size() >= config::kMaxBookmarks;
    ImGui::BeginDisabled(full);
    if (ImGui::SmallButton("Bookmark")) addBookmark(p.label, p.glyph, p.pos);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(full ? "You have %d bookmarks already: delete one first" : "Saves this spot, icon and title to your bookmarks",
                          static_cast<int>(config::kMaxBookmarks));
}

void placedList(const config::Settings& cfg, const std::vector<net::PingInfo>& pings, const float* me) {
    int count = 0;
    for (const auto& p : pings) count += p.own ? 1 : 0;
    char head[48];
    snprintf(head, sizeof(head), "Placed markers (%d)###placed", count);
    if (!ImGui::TreeNode(head)) return;
    if (!count) ImGui::TextDisabled("You have no markers placed.");
    for (const auto& p : pings) {
        if (!p.own) continue;
        ImGui::PushID(p.index);
        rowIcon(p.glyph, p.rgb, p.accent);
        ImGui::AlignTextToFramePadding();
        if (g_renaming == Renaming::Placed && g_renameIndex == p.index) {
            std::string title;
            if (renameField(title)) net::renamePing(p.index, title);
        } else {
            ImGui::TextUnformatted(titleOr(p.label));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", whereAndLeft(p, me).c_str());
            ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::SmallButton("Move")) game::ping::requestMove(p.index);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves this marker to where a new one would go now");
            ImGui::SameLine();
            if (ImGui::SmallButton("Renew")) net::renewPing(p.index);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starts this marker's time again");
            ImGui::SameLine();
            if (ImGui::SmallButton("Rename")) beginRename(Renaming::Placed, p.index, p.label);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Changes the title of this marker only");
            ImGui::SameLine();
            bookmarkButton(cfg, p);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) net::removePing(p.index);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes this marker for everyone");
            ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void bookmarkList(const config::Settings& cfg, const float* me) {
    char head[48];
    snprintf(head, sizeof(head), "Bookmarks (%d)###bookmarks", static_cast<int>(cfg.bookmarks.size()));
    if (!ImGui::TreeNode(head)) return;
    if (cfg.bookmarks.empty()) ImGui::TextDisabled("Nothing saved yet. \"Bookmark\" on a marker saves its spot.");
    uint8_t rgb[3] = {255, 210, 60}, accent[3] = {255, 255, 255};
    net::protocol::parseRgb(cfg.pingColor, rgb);
    net::protocol::parseRgb(cfg.pingAccent, accent);
    for (int i = 0; i < static_cast<int>(cfg.bookmarks.size()); ++i) {
        const config::Bookmark& b = cfg.bookmarks[static_cast<size_t>(i)];
        ImGui::PushID(i);
        rowIcon(b.glyph, rgb, accent);
        ImGui::AlignTextToFramePadding();
        if (g_renaming == Renaming::Bookmark && g_renameIndex == i) {
            std::string title;
            if (renameField(title))
                config::change([i, title](config::Settings& c) {
                    if (i < static_cast<int>(c.bookmarks.size())) c.bookmarks[static_cast<size_t>(i)].label = title;
                });
        } else {
            ImGui::TextUnformatted(titleOr(b.label));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", distanceText(b.pos, me).c_str());
            ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::SmallButton("Place")) {
                net::PingStyle style;
                memcpy(style.rgb, rgb, sizeof(rgb));
                memcpy(style.accent, accent, sizeof(accent));
                style.glyph = b.glyph;
                style.label = b.label;
                style.seconds = cfg.pingSeconds;
                net::sendPing(b.pos, style, cfg.pingMax);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Places a marker here for everyone to see");
            ImGui::SameLine();
            if (ImGui::SmallButton("Rename")) beginRename(Renaming::Bookmark, i, b.label);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                g_renaming = Renaming::None; // the rows below move up
                config::change([i](config::Settings& c) {
                    if (i < static_cast<int>(c.bookmarks.size())) c.bookmarks.erase(c.bookmarks.begin() + i);
                });
            }
            ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void othersList(const config::Settings& cfg, const std::vector<net::PingInfo>& pings, const float* me) {
    int count = 0;
    for (const auto& p : pings) count += p.own ? 0 : 1;
    char head[56];
    snprintf(head, sizeof(head), "Other placed markers (%d)###others", count);
    if (!ImGui::TreeNode(head)) return;
    if (!count) ImGui::TextDisabled("Nobody else has a marker placed.");
    for (const auto& p : pings) {
        if (p.own) continue;
        ImGui::PushID(static_cast<int>(p.owner));
        ImGui::PushID(p.index);
        rowIcon(p.glyph, p.rgb, p.accent);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s: %s", p.name.c_str(), titleOr(p.label));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", whereAndLeft(p, me).c_str());
        ImGui::SameLine();
        bookmarkButton(cfg, p);
        ImGui::PopID();
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void markerLists(const config::Settings& cfg) {
    static net::PlayerState local; // large; render thread only
    const float* me = net::localState(local) ? local.pos : nullptr;
    const auto pings = net::pings();
    placedList(cfg, pings, me);
    bookmarkList(cfg, me);
    othersList(cfg, pings, me);
}

void markersSection() {
    const auto cfgPtr = config::get();
    const config::Settings& cfg = *cfgPtr;
    markerLists(cfg);
    bool showPings = cfg.showPings;
    if (ImGui::Checkbox("Show markers", &showPings)) config::change([showPings](config::Settings& c) { c.showPings = showPings; });
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shows your markers and everyone else's. A marker that is off screen becomes an arrow\n"
                          "on the edge of the screen.");
    ImGui::SameLine();
    markerColor("##pingcolor", cfg.pingColor, &config::Settings::pingColor, "Your marker's color", 255);
    ImGui::SameLine();
    markerColor("##pingaccent", cfg.pingAccent, &config::Settings::pingAccent, "Your secondary marker's color", 255);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear my markers")) net::clearPing();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes all your markers for everyone");
    bool showArrows = cfg.showArrows;
    if (ImGui::Checkbox("Player arrows", &showArrows)) config::change([showArrows](config::Settings& c) { c.showArrows = showArrows; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows an arrow on the edge of the screen towards each player you cannot see");
    bool hideAll = ui::overlayHidden();
    if (ImGui::Checkbox("Hide names, markers, arrows and notices", &hideAll)) ui::setOverlayHidden(hideAll);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("For a screenshot or a recording. The hide key (Controls tab) does the same.");
    bool pingPulse = cfg.pingPulse;
    if (ImGui::Checkbox("Flash markers when placed", &pingPulse))
        config::change([pingPulse](config::Settings& c) { c.pingPulse = pingPulse; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New and moved markers blink for a split second so they are easier to spot");
    const bool canAim = game::raycast::available();
    bool pingAim = cfg.pingAim && canAim;
    ImGui::BeginDisabled(!canAim);
    if (ImGui::Checkbox("Place markers where I look", &pingAim))
        config::change([pingAim](config::Settings& c) { c.pingAim = pingAim; });
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(canAim ? "Markers land on what the center of your screen points at, up to 300 m away.\n"
                                   "Off, or with nothing in reach: where you stand."
                                 : "Not available in this version of the game: markers go where you stand");

    // How long a marker stays (the wire carries it in steps of 5 s).
    const int shown = g_lifeEdit.editing ? g_lifeEdit.value : cfg.pingSeconds;
    char life[32];
    if (shown < 60) snprintf(life, sizeof(life), "%d s", shown);
    else if (shown % 60) snprintf(life, sizeof(life), "%d min %d s", shown / 60, shown % 60);
    else snprintf(life, sizeof(life), "%d min", shown / 60);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (settingSlider("Marker lifetime", g_lifeEdit, cfg.pingSeconds, 5, 300, life, ImGuiSliderFlags_Logarithmic)) {
        const int seconds = net::protocol::pingLifeUnits(g_lifeEdit.value) * 5;
        g_lifeEdit.value = seconds;
        config::change([seconds](config::Settings& c) { c.pingSeconds = seconds; });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long your markers last: 5 seconds to 5 minutes");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (settingSlider("Markers at once", g_maxEdit, cfg.pingMax, 1, net::protocol::kPingCells, "%d", 0)) {
        const int n = g_maxEdit.value;
        config::change([n](config::Settings& c) { c.pingMax = n; });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How many markers you can have at the same time. Your oldest one is removed when you go\n"
                          "over the limit. Placing the same marker within 1.5 m of one of yours moves it instead.");

    ImGui::TextDisabled("The marker keys and buttons are on the Controls tab.");
    if (ImGui::TreeNode("Marker wheels")) {
        if (ImGui::BeginTabBar("wheels")) {
            for (int w = 0; w < cfg.wheelCount; ++w) {
                const std::string tab = (cfg.wheelNames[w].empty() ? "Wheel " + std::to_string(w + 1) : cfg.wheelNames[w]) +
                                        "###wheel" + std::to_string(w);
                if (ImGui::BeginTabItem(tab.c_str())) {
                    wheelEditor(cfg, w);
                    ImGui::EndTabItem();
                }
            }
            if (cfg.wheelCount < config::kWheels) {
                if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
                    loadLabels(cfg.wheelCount, config::defaultWheel(cfg.wheelCount));
                    g_wheelName[cfg.wheelCount][0] = 0;
                    config::change([](config::Settings& c) {
                        if (c.wheelCount >= config::kWheels) return;
                        c.wheels[c.wheelCount] = config::defaultWheel(c.wheelCount);
                        c.wheelSlots[c.wheelCount] = config::kPingSlots;
                        c.wheelNames[c.wheelCount].clear();
                        ++c.wheelCount;
                    });
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a wheel (up to %d)", config::kWheels);
            }
            ImGui::EndTabBar();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Quick answers")) {
        ImGui::TextDisabled("Look at another player's marker, then tap or hold %s: the owner gets your answer.",
                            cfg.reactKey.empty() ? "the react key" : ui::keyLabel(cfg.reactKey).c_str());
        reactionEditor(cfg);
        ImGui::TreePop();
    }
}

void controlsSection() {
    const auto cfg = config::get();
    ImGui::SeparatorText("Menu");
    // Keyboard key that opens the menu.
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Menu key:");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The key that opens and closes this menu");
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
        ImGui::BeginDisabled(g_bindCapture >= 0); // one key is recorded at a time
        if (ImGui::SmallButton("Change##key")) {
            g_keyCapture = true;
            ui::setKeyCapture(true);
        }
        ImGui::EndDisabled();
    }
    // Gamepad buttons that open it.
    padRow("Gamepad:", gamepad::Action::Menu, cfg->menuPad, &config::Settings::menuPad,
           "Buttons held together to open and close this menu");

    ImGui::SeparatorText("Chat");
    keyRow(*cfg, 2);

    ImGui::SeparatorText("Markers");
    ImGui::TextUnformatted("Place a marker / open the wheel");
    ImGui::Indent();
    keyRow(*cfg, 0);
    padRow("Gamepad:", gamepad::Action::Ping, cfg->pingPad, &config::Settings::pingPad,
           "The same on a gamepad: tap to place and hold for the wheel. The right stick chooses.");
    ImGui::Unindent();
    ImGui::TextUnformatted("Clear markers");
    ImGui::Indent();
    keyRow(*cfg, 1);
    padRow("Gamepad:", gamepad::Action::PingClear, cfg->pingClearPad, &config::Settings::pingClearPad,
           "While you hold the marker button: removes all your markers.");
    ImGui::Unindent();
    ImGui::TextUnformatted("Place the other way");
    ImGui::Indent();
    keyRow(*cfg, 4);
    ImGui::Unindent();
    tapModeRows(*cfg);

    ImGui::TextUnformatted("The marker wheel");
    ImGui::Indent();
    if (ImGui::RadioButton("opens after a short hold", !cfg->wheelInstant))
        config::change([](config::Settings& c) { c.wheelInstant = false; });
    if (ImGui::RadioButton("opens the moment the key goes down", cfg->wheelInstant))
        config::change([](config::Settings& c) { c.wheelInstant = true; });
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A very short tap that does not move the pointer still places your marker.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    float sens = g_sensEdit.editing ? g_sensEdit.value : cfg->wheelSensitivity;
    if (ImGui::SliderFloat("Pointer speed", &sens, 0.1f, 3.f, "%.2f x", ImGuiSliderFlags_AlwaysClamp)) {
        g_sensEdit.value = sens;
        g_sensEdit.editing = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const float v = g_sensEdit.value;
        config::change([v](config::Settings& c) { c.wheelSensitivity = v; });
    }
    if (g_sensEdit.editing && !ImGui::IsItemActive() && std::fabs(cfg->wheelSensitivity - g_sensEdit.value) < 0.001f)
        g_sensEdit.editing = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far the pointer in the open wheel moves per mouse movement. Raise it for a low mouse sensitivity.");
    ImGui::TextUnformatted("Previous wheel (while it is open)");
    keyRow(*cfg, 6);
    ImGui::TextUnformatted("Next wheel (while it is open)");
    keyRow(*cfg, 7);
    ImGui::Unindent();

    ImGui::SeparatorText("Quick answers");
    keyRow(*cfg, 3);
    padRow("Gamepad:", gamepad::Action::React, cfg->reactPad, &config::Settings::reactPad,
           "The same on a gamepad: tap sends your last answer, hold opens the answers wheel.");

    ImGui::SeparatorText("Overlay");
    ImGui::TextUnformatted("Show / hide names, markers, arrows, notices and chat");
    ImGui::Indent();
    keyRow(*cfg, 5);
    bool hidden = ui::overlayHidden();
    if (ImGui::Checkbox("Hidden now", &hidden)) ui::setOverlayHidden(hidden);
    ImGui::Unindent();

    ImGui::SeparatorText("Gamepad");
    bool ps = cfg->padPlayStation;
    if (ImGui::Checkbox("Name the buttons as on a PlayStation pad", &ps))
        config::change([ps](config::Settings& c) { c.padPlayStation = ps; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only the names in hints change (A = Cross, LB = L1, ...). Your pad works the same.");
}

// "INSERT or LS+RS"
std::string openKeys(const config::Settings& cfg) {
    const uint16_t pad = gamepad::parse(cfg.menuPad);
    return pad ? cfg.menuKey + " or " + gamepad::labelCombo(gamepad::format(pad)) : cfg.menuKey;
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
        for (int w = 0; w < config::kWheels; ++w) {
            loadLabels(w, config::get()->wheels[w]);
            snprintf(g_wheelName[w], sizeof(g_wheelName[w]), "%s", config::get()->wheelNames[w].c_str());
        }
        for (int i = 0; i < config::kPingSlots; ++i)
            snprintf(g_reactLabel[i], sizeof(g_reactLabel[i]), "%s", config::get()->reactions[i].label.c_str());
        g_hintUntil = GetTickCount() + 10000;
    }
    static bool wasOpen = false;
    if (wasOpen && !open) net::servers::update(net::servers::split(config::get()->servers), false); // stop asking
    wasOpen = open;
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
    // One size for all tabs (the longer ones scroll), never taller than the screen.
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, vp->WorkSize.y * 0.7f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, vp->WorkSize.y * 0.94f));
    char title[64];
    snprintf(title, sizeof(title), "MECMP %s###mecmp", app::kVersion);
    ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Session")) {
            sessionSection();
            serversSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Players")) {
            playersSection();
            myLookSection();
            lookSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Markers")) {
            markersSection();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            controlsSection();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s closes this menu", openKeys(*config::get()).c_str());
    ImGui::End();
}

} // namespace mecmp::ui::menu
