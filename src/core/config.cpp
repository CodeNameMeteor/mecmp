#include "core/config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "net/protocol.h"

namespace mecmp::config {
namespace {
std::mutex g_m;             // guards g_settings and g_queue
// The current settings, never changed once published: readers keep the snapshot they got.
std::shared_ptr<const Settings> g_settings = std::make_shared<const Settings>();
struct Change { std::function<void(Settings&)> edit; bool persist; };
std::vector<Change> g_queue;
std::wstring g_dir;

constexpr const char* kDefaultIni =
    "; MECMP settings (read when the mod loads)\r\n"
    "[net]\r\n"
    "enabled=1\r\n"
    "; UDP port this game uses\r\n"
    "port=47800\r\n"
    "; HOST: leave join empty. The mod opens the port on your router (UPnP) and writes your\r\n"
    ";       join code to the log and to join_code.txt (the menu's Copy button copies it).\r\n"
    "; JOIN: paste the host's join code, e.g. join=8H3KQ-ZT1V4 (or the host's ip:port).\r\n"
    "; solo test: join=127.0.0.1:47800 (a ghost mirrors you)\r\n"
    "join=\r\n"
    "; the menu's server list: dedicated servers' addresses, comma separated (the menu edits it)\r\n"
    "servers=\r\n"
    "; solo test only: how far the mirrored ghost stands from you, along the world X axis (metres)\r\n"
    "loopback_offset=3.0\r\n"
    "\r\n"
    "[player]\r\n"
    "; your name, shown to the other players\r\n"
    "name=Runner\r\n"
    "; how the other players see you: faith, or a ghost style and color (pick it in the in-game menu)\r\n"
    "look=faith\r\n"
    "; color of the markers you place, as the others see them: RRGGBB\r\n"
    "ping_color=ffd23c\r\n"
    "; color of the small diamond inside the diamond marker\r\n"
    "ping_accent=ffffff\r\n"
    "; how long your markers stay, in seconds (5..300), and how many you can have at once (1..5)\r\n"
    "ping_seconds=30\r\n"
    "ping_max=3\r\n"
    "; the marker wheels (hold the marker key; scroll or the Left/Right arrows switch wheel): the title and\r\n"
    "; icon of each of the nine slots, as the others see them. A title is at most 32 bytes; empty = a\r\n"
    "; marker without a title. Icons: diamond chevron danger bars up flag star ring down left right cross\r\n"
    "; check question crosshair bolt plus noentry heart magrope minus target eye dot jump slide ladder\r\n"
    "; zipline door wallrun shield bubble vault\r\n"
    "; wheel_count = how many wheels there are (3..5); wheels 4 and 5 are added in the menu\r\n"
    "wheel_count=3\r\n";
// The wheel lines (written from the defaults) go between the two halves.
constexpr const char* kDefaultIniTail =
    "\r\n"
    "[ghost]\r\n"
    "; spawn other players' ghosts automatically when their packets arrive\r\n"
    "auto_spawn=1\r\n"
    "\r\n"
    "[voice]\r\n"
    "; 1 = positional voice through Mumble: players in the same session hear each other from where they\r\n"
    "; are (needs Mumble running with Positional Audio and its Link plugin on). 0 = off\r\n"
    "mumble_link=1\r\n"
    "\r\n"
    "[ui]\r\n"
    "; key that opens the in-game menu: INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, PAUSE or F1..F12\r\n"
    "menu_key=INSERT\r\n"
    "; gamepad buttons that open it, held together: A B X Y LB RB LS RS BACK START UP DOWN LEFT RIGHT\r\n"
    "; joined with +, e.g. LS+RS or BACK+Y. Empty = no gamepad button. Both can be changed in the menu.\r\n"
    "menu_pad=LS+RS\r\n"
    "; 1 = show player names above their ghosts\r\n"
    "show_names=1\r\n"
    "; 1 = show a short notice when a player joins, leaves or places a marker\r\n"
    "show_notices=1\r\n"
    "; 1 = show chat messages; chat_key opens the chat line (a letter A..Z, ENTER, or INSERT/F1../...)\r\n"
    "show_chat=1\r\n"
    "chat_key=ENTER\r\n"
    "; markers: a short press of ping_key places one where you stand, holding it opens the marker wheel\r\n"
    "; (a letter A..Z, ENTER, or INSERT/F1../...; empty = no key). ping_pad = gamepad buttons for the same\r\n"
    "; (as menu_pad; empty = none). Both can be changed in the menu.\r\n"
    "ping_key=G\r\n"
    "ping_pad=\r\n"
    "; what a short press places: ping_tap=0 the marker you picked last, ping_tap=1 always the one in\r\n"
    "; ping_tap_wheel (1..5) / ping_tap_slot (1..9)\r\n"
    "ping_tap=0\r\n"
    "ping_tap_wheel=1\r\n"
    "ping_tap_slot=1\r\n"
    "; removing all your markers: hold ping_clear_key for a moment (a key as ping_key, or MOUSE3 = middle\r\n"
    "; button, MOUSE4, MOUSE5; in the open wheel, holding the middle button does the same);\r\n"
    "; ping_clear_pad = gamepad buttons, pressed while the marker buttons are held. Empty = none\r\n"
    "ping_clear_key=H\r\n"
    "ping_clear_pad=RS\r\n"
    "; 1 = show the players' markers\r\n"
    "show_pings=1\r\n"
    "; 1 = show an arrow on the screen's edge towards each player who is off screen\r\n"
    "show_arrows=1\r\n"
    "; 1 = a marker flashes for a moment when it arrives or moves (0 = no animation)\r\n"
    "ping_pulse=1\r\n"
    "; 1 = a marker goes where you look (up to 300 m away), 0 = where you stand\r\n"
    "ping_aim=1\r\n"
    "\r\n"
    "[bookmarks]\r\n"
    "; spots saved from markers in the menu: count, then b1.. = icon|x|y|z|title (x, y, z in centimetres)\r\n"
    "count=0\r\n"
    "\r\n"
    "[debug]\r\n"
    "; 1 = detailed diagnostic log (for bug reports)\r\n"
    "verbose=0\r\n";

std::string readString(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const char* def) {
    wchar_t w[2048] = {};
    GetPrivateProfileStringW(sec, key, L"\x01", w, 2048, ini.c_str());
    if (w[0] == 1) return def;
    char buf[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}
int readInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(sec, key, def, ini.c_str()));
}
int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const char* const kGlyphNames[kPingGlyphs] = {
    "diamond", "chevron",  "danger",    "bars", "up",   "flag",    "star",  "ring",   "down",     "left",   "right", "cross",
    "check",   "question", "crosshair", "bolt", "plus", "noentry", "heart", "magrope", "minus",   "target", "eye",   "dot",
    "jump",    "slide",    "ladder",    "zipline", "door", "wallrun", "shield", "bubble",
    "vault"};

// A bookmark as an ini value: icon|x|y|z|title. The position is in whole centimetres, so reading it
// back does not depend on how the system writes decimals; the title comes last and may contain '|'.
std::string formatBookmark(const Bookmark& b) {
    char head[96];
    snprintf(head, sizeof(head), "%s|%ld|%ld|%ld|", glyphName(b.glyph), std::lround(b.pos[0] * 100.f), std::lround(b.pos[1] * 100.f),
             std::lround(b.pos[2] * 100.f));
    return head + b.label;
}
bool parseBookmark(const std::string& text, Bookmark& out) {
    size_t bar[4];
    size_t from = 0;
    for (size_t& at : bar) {
        at = text.find('|', from);
        if (at == std::string::npos) return false;
        from = at + 1;
    }
    Bookmark b;
    b.glyph = glyphFromName(text.substr(0, bar[0]), 0);
    for (int i = 0; i < 3; ++i) b.pos[i] = static_cast<float>(strtol(text.c_str() + bar[i] + 1, nullptr, 10)) / 100.f;
    if (!net::protocol::sanePosition(b.pos)) return false;
    b.label = net::protocol::cleanPingLabel(text.substr(bar[3] + 1));
    out = b;
    return true;
}

// "wheel1_label_3"
std::wstring wheelKey(int wheel, const wchar_t* what, int slot) {
    return L"wheel" + std::to_wstring(wheel + 1) + L"_" + what + L"_" + std::to_wstring(slot + 1);
}
} // namespace

const char* glyphName(uint8_t glyph) { return kGlyphNames[glyph < kPingGlyphs ? glyph : 0]; }

uint8_t glyphFromName(const std::string& name, uint8_t fallback) {
    for (int i = 0; i < kPingGlyphs; ++i)
        if (_stricmp(name.c_str(), kGlyphNames[i]) == 0) return static_cast<uint8_t>(i);
    return fallback;
}

Wheel defaultWheel(int wheel) {
    if (wheel == 0) // moving around
        return {{{"Here", 0}, {"Go this way", 1}, {"Springboard", 24}, {"Jump here", 4}, {"Climb here", 26},
                 {"Slide under", 25}, {"Vault over", 32}, {"Wallrun", 29}, {"Drop down", 8}}};
    if (wheel == 1) // the route, danger, and talking to each other
        return {{{"Through here", 28}, {"Start", 5}, {"Finish", 21}, {"Danger", 2}, {"Follow me", 31},
                 {"Wait", 3}, {"Look here", 22}, {"Help!", 13}, {"Nice!", 6}}};
    Wheel empty; // a wheel the player added: theirs to fill
    for (WheelSlot& s : empty) s.glyph = 23;
    if (wheel == 2) { // things to use; the rest of this wheel is the player's to fill
        empty[0] = {"Zipline", 27};
        empty[1] = {"Magrope", 19};
        empty[2] = {"Shortcut", 15};
    }
    return empty;
}

std::array<Wheel, kWheels> defaultWheels() {
    std::array<Wheel, kWheels> all;
    for (int w = 0; w < kWheels; ++w) all[w] = defaultWheel(w);
    return all;
}

void load(const std::wstring& dir) {
    std::wstring d = dir;
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    g_dir = d;
    const std::wstring ini = d + L"mecmp.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            fputs(kDefaultIni, f);
            for (int w = 0; w < kDefaultWheels; ++w) {
                const Wheel wheel = defaultWheel(w);
                for (int i = 0; i < kPingSlots; ++i)
                    fprintf(f, "wheel%d_label_%d=%s\r\nwheel%d_icon_%d=%s\r\n", w + 1, i + 1, wheel[i].label.c_str(), w + 1,
                            i + 1, glyphName(wheel[i].glyph));
            }
            fputs(kDefaultIniTail, f);
            fclose(f);
            MLOG("config: wrote default %ls", ini.c_str());
        }
    }
    auto built = std::make_shared<Settings>();
    Settings& s = *built;
    s.netEnabled = readInt(ini, L"net", L"enabled", 1) != 0;
    const int port = readInt(ini, L"net", L"port", 47800);
    s.port = port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : 47800; // 0 and out-of-range ports are not usable
    s.join = readString(ini, L"net", L"join", "\x01");
    if (s.join == "\x01") s.join = readString(ini, L"net", L"peer", ""); // older mecmp.ini files (no join= line)
    s.servers = readString(ini, L"net", L"servers", "");
    const float offset = static_cast<float>(atof(readString(ini, L"net", L"loopback_offset", "3.0").c_str()));
    s.loopbackOffset = std::isfinite(offset) ? offset : 3.0f; // "nan" or "inf" would poison the mirrored position
    s.name = readString(ini, L"player", L"name", "Runner");
    if (s.name.empty()) s.name = "Runner";
    s.name.resize(net::protocol::utf8PrefixLength(s.name, net::protocol::kMaxNameBytes)); // what goes on the wire
    s.autoSpawn = readInt(ini, L"ghost", L"auto_spawn", 1) != 0;
    s.look = readString(ini, L"player", L"look", "faith");
    // The [ghost] model setting is gone: each player picks their own look ([player] look=) in the menu.
    const std::string legacyModel = readString(ini, L"ghost", L"model", "");
    if (!legacyModel.empty() && legacyModel != "faith")
        MLOG("config: [ghost] model=%s is no longer used; pick your look in the menu", legacyModel.c_str());
    s.menuKey = readString(ini, L"ui", L"menu_key", "INSERT");
    s.menuPad = readString(ini, L"ui", L"menu_pad", "LS+RS");
    s.showNames = readInt(ini, L"ui", L"show_names", 1) != 0;
    s.mumbleLink = readInt(ini, L"voice", L"mumble_link", 1) != 0;
    s.showNotices = readInt(ini, L"ui", L"show_notices", 1) != 0;
    s.showChat = readInt(ini, L"ui", L"show_chat", 1) != 0;
    s.chatKey = readString(ini, L"ui", L"chat_key", "ENTER");
    s.pingKey = readString(ini, L"ui", L"ping_key", "G");
    s.pingPad = readString(ini, L"ui", L"ping_pad", "");
    s.showPings = readInt(ini, L"ui", L"show_pings", 1) != 0;
    s.showArrows = readInt(ini, L"ui", L"show_arrows", 1) != 0;
    s.pingPulse = readInt(ini, L"ui", L"ping_pulse", 1) != 0;
    s.pingAim = readInt(ini, L"ui", L"ping_aim", 1) != 0;
    s.pingColor = readString(ini, L"player", L"ping_color", "ffd23c");
    s.pingAccent = readString(ini, L"player", L"ping_accent", "ffffff");
    s.pingSeconds = clampInt(readInt(ini, L"player", L"ping_seconds", 30), 5, 300);
    s.pingMax = clampInt(readInt(ini, L"player", L"ping_max", 3), 1, net::protocol::kPingCells);
    s.wheelCount = clampInt(readInt(ini, L"player", L"wheel_count", kDefaultWheels), kDefaultWheels, kWheels);
    for (int w = 0; w < s.wheelCount; ++w) {
        const Wheel defaults = defaultWheel(w);
        for (int i = 0; i < kPingSlots; ++i) {
            std::string label = readString(ini, L"player", wheelKey(w, L"label", i).c_str(), "\x01");
            // A file from the first wheel version: one wheel, its titles in ping_label_N.
            if (label == "\x01" && w == 0)
                label = readString(ini, L"player", (L"ping_label_" + std::to_wstring(i + 1)).c_str(), "\x01");
            if (label == "\x01") label = defaults[i].label;
            s.wheels[w][i].label = net::protocol::cleanPingLabel(label);
            s.wheels[w][i].glyph =
                glyphFromName(readString(ini, L"player", wheelKey(w, L"icon", i).c_str(), ""), defaults[i].glyph);
        }
    }
    s.pingWheel = clampInt(readInt(ini, L"ui", L"ping_wheel", 1), 1, s.wheelCount) - 1;
    s.pingLastWheel = clampInt(readInt(ini, L"ui", L"ping_last_wheel", 1), 1, s.wheelCount) - 1;
    s.pingLastSlot = clampInt(readInt(ini, L"ui", L"ping_last_slot", 1), 1, kPingSlots) - 1;
    s.pingTap = readInt(ini, L"ui", L"ping_tap", 0) != 0 ? 1 : 0;
    s.pingFixedWheel = clampInt(readInt(ini, L"ui", L"ping_tap_wheel", 1), 1, s.wheelCount) - 1;
    s.pingFixedSlot = clampInt(readInt(ini, L"ui", L"ping_tap_slot", 1), 1, kPingSlots) - 1;
    s.pingClearKey = readString(ini, L"ui", L"ping_clear_key", "H");
    s.pingClearPad = readString(ini, L"ui", L"ping_clear_pad", "RS");
    {
        const int count = clampInt(readInt(ini, L"bookmarks", L"count", 0), 0, static_cast<int>(kMaxBookmarks));
        for (int i = 0; i < count; ++i) {
            Bookmark b;
            if (parseBookmark(readString(ini, L"bookmarks", (L"b" + std::to_wstring(i + 1)).c_str(), ""), b)) s.bookmarks.push_back(b);
        }
    }
    s.verbose = readInt(ini, L"debug", L"verbose", 0) != 0;
    log::verbose = s.verbose;
    s.ignoreBuildCheck = readInt(ini, L"debug", L"ignore_build_check", 0) != 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_settings = built;
    }
    MLOG("config: name '%s', port %u, %s '%s', look '%s'%s", s.name.c_str(), s.port,
         s.join.empty() ? "hosting" : "joining", s.join.c_str(), s.look.c_str(),
         s.verbose ? ", verbose log" : "");
}

uint32_t lookHash(const std::string& look) {
    if (look.rfind("echo:", 0) != 0) return 0;
    return static_cast<uint32_t>(strtoul(look.c_str() + 5, nullptr, 16));
}

std::string lookName(uint32_t hash) {
    if (!hash) return "faith";
    char b[16];
    snprintf(b, sizeof(b), "echo:%08x", hash);
    return b;
}

std::shared_ptr<const Settings> get() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_settings;
}
const std::wstring& dir() { return g_dir; }

void change(std::function<void(Settings&)> edit, bool persist) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_queue.size() < 256) g_queue.push_back({std::move(edit), persist});
}

void applyPending() {
    std::vector<Change> todo;
    std::shared_ptr<const Settings> snapshot;
    bool persist = false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_queue.empty()) return;
        todo.swap(g_queue);
        auto next = std::make_shared<Settings>(*g_settings); // copy-on-write: published, never edited in place
        for (auto& c : todo) {
            c.edit(*next);
            persist = persist || c.persist;
        }
        g_settings = next;
        snapshot = next;
    }
    if (!persist) return;
    const std::wstring ini = g_dir + L"mecmp.ini";
    auto put = [&](const wchar_t* sec, const wchar_t* key, const std::string& v) {
        wchar_t w[2048] = {};
        MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, w, 2048);
        WritePrivateProfileStringW(sec, key, w, ini.c_str());
    };
    put(L"player", L"name", snapshot->name);
    put(L"net", L"join", snapshot->join);
    put(L"net", L"servers", snapshot->servers);
    put(L"player", L"look", snapshot->look);
    put(L"ui", L"menu_key", snapshot->menuKey);
    put(L"ui", L"menu_pad", snapshot->menuPad);
    put(L"ui", L"show_names", snapshot->showNames ? "1" : "0");
    put(L"voice", L"mumble_link", snapshot->mumbleLink ? "1" : "0");
    put(L"ui", L"show_notices", snapshot->showNotices ? "1" : "0");
    put(L"ui", L"show_chat", snapshot->showChat ? "1" : "0");
    put(L"ui", L"chat_key", snapshot->chatKey);
    put(L"ui", L"ping_key", snapshot->pingKey);
    put(L"ui", L"ping_pad", snapshot->pingPad);
    put(L"ui", L"show_pings", snapshot->showPings ? "1" : "0");
    put(L"ui", L"show_arrows", snapshot->showArrows ? "1" : "0");
    put(L"ui", L"ping_pulse", snapshot->pingPulse ? "1" : "0");
    put(L"ui", L"ping_aim", snapshot->pingAim ? "1" : "0");
    put(L"player", L"ping_color", snapshot->pingColor);
    put(L"player", L"ping_accent", snapshot->pingAccent);
    put(L"player", L"ping_seconds", std::to_string(snapshot->pingSeconds));
    put(L"player", L"ping_max", std::to_string(snapshot->pingMax));
    put(L"player", L"wheel_count", std::to_string(snapshot->wheelCount));
    for (int w = 0; w < snapshot->wheelCount && w < kWheels; ++w)
        for (int i = 0; i < kPingSlots; ++i) {
            put(L"player", wheelKey(w, L"label", i).c_str(), snapshot->wheels[w][i].label);
            put(L"player", wheelKey(w, L"icon", i).c_str(), glyphName(snapshot->wheels[w][i].glyph));
        }
    put(L"ui", L"ping_wheel", std::to_string(snapshot->pingWheel + 1));
    put(L"ui", L"ping_last_wheel", std::to_string(snapshot->pingLastWheel + 1));
    put(L"ui", L"ping_last_slot", std::to_string(snapshot->pingLastSlot + 1));
    put(L"ui", L"ping_tap", std::to_string(snapshot->pingTap));
    put(L"ui", L"ping_tap_wheel", std::to_string(snapshot->pingFixedWheel + 1));
    put(L"ui", L"ping_tap_slot", std::to_string(snapshot->pingFixedSlot + 1));
    put(L"ui", L"ping_clear_key", snapshot->pingClearKey);
    put(L"ui", L"ping_clear_pad", snapshot->pingClearPad);
    put(L"bookmarks", L"count", std::to_string(snapshot->bookmarks.size()));
    for (size_t i = 0; i < snapshot->bookmarks.size(); ++i)
        put(L"bookmarks", (L"b" + std::to_wstring(i + 1)).c_str(), formatBookmark(snapshot->bookmarks[i]));
    MLOG("config: saved");
}

} // namespace mecmp::config
