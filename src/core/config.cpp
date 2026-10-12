#include "core/config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    "; solo test only: how far the mirrored ghost stands from you, along the world X axis (metres)\r\n"
    "loopback_offset=3.0\r\n"
    "\r\n"
    "[player]\r\n"
    "; your name, shown to the other players\r\n"
    "name=Runner\r\n"
    "; how the other players see you: faith, or a ghost style and colour (pick it in the in-game menu)\r\n"
    "look=faith\r\n"
    "\r\n"
    "[ghost]\r\n"
    "; spawn other players' ghosts automatically when their packets arrive\r\n"
    "auto_spawn=1\r\n"
    "\r\n"
    "[ui]\r\n"
    "; key that opens the in-game menu: INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, PAUSE or F1..F12\r\n"
    "menu_key=INSERT\r\n"
    "; gamepad buttons that open it, held together: A B X Y LB RB LS RS BACK START UP DOWN LEFT RIGHT\r\n"
    "; joined with +, e.g. LS+RS or BACK+Y. Empty = no gamepad button. Both can be changed in the menu.\r\n"
    "menu_pad=LS+RS\r\n"
    "; 1 = show player names above their ghosts\r\n"
    "show_names=1\r\n"
    "; 1 = show a short notice when a player joins or leaves\r\n"
    "show_notices=1\r\n"
    "\r\n"
    "[debug]\r\n"
    "; 1 = detailed diagnostic log (for bug reports)\r\n"
    "verbose=0\r\n";

std::string readString(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const char* def) {
    wchar_t w[256] = {};
    GetPrivateProfileStringW(sec, key, L"\x01", w, 256, ini.c_str());
    if (w[0] == 1) return def;
    char buf[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}
int readInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(sec, key, def, ini.c_str()));
}
} // namespace

void load(const std::wstring& dir) {
    std::wstring d = dir;
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    g_dir = d;
    const std::wstring ini = d + L"mecmp.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            fputs(kDefaultIni, f);
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
    const float offset = static_cast<float>(atof(readString(ini, L"net", L"loopback_offset", "3.0").c_str()));
    s.loopbackOffset = std::isfinite(offset) ? offset : 3.0f; // "nan" or "inf" would poison the mirrored position
    s.name = readString(ini, L"player", L"name", "Runner");
    if (s.name.empty()) s.name = "Runner";
    s.name.resize(net::protocol::utf8PrefixLength(s.name, net::protocol::kMaxNameBytes)); // what goes on the wire
    s.autoSpawn = readInt(ini, L"ghost", L"auto_spawn", 1) != 0;
    s.look = readString(ini, L"player", L"look", "faith");
    s.menuKey = readString(ini, L"ui", L"menu_key", "INSERT");
    s.menuPad = readString(ini, L"ui", L"menu_pad", "LS+RS");
    s.showNames = readInt(ini, L"ui", L"show_names", 1) != 0;
    s.showNotices = readInt(ini, L"ui", L"show_notices", 1) != 0;
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
        wchar_t w[512] = {};
        MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, w, 512);
        WritePrivateProfileStringW(sec, key, w, ini.c_str());
    };
    put(L"player", L"name", snapshot->name);
    put(L"net", L"join", snapshot->join);
    put(L"player", L"look", snapshot->look);
    put(L"ui", L"menu_key", snapshot->menuKey);
    put(L"ui", L"menu_pad", snapshot->menuPad);
    put(L"ui", L"show_names", snapshot->showNames ? "1" : "0");
    put(L"ui", L"show_notices", snapshot->showNotices ? "1" : "0");
    MLOG("config: saved");
}

} // namespace mecmp::config
