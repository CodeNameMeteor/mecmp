#include "core/config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "core/log.h"

namespace mecmp::config {
namespace {
Settings g_settings;

constexpr const char* kDefaultIni =
    "; MECMP settings (read when the mod loads)\r\n"
    "[net]\r\n"
    "enabled=1\r\n"
    "; UDP port this game listens on\r\n"
    "port=47800\r\n"
    "; the other player's address, e.g. peer=192.168.0.144:47800\r\n"
    "; leave empty to wait until the other player contacts you\r\n"
    "; solo test: peer=127.0.0.1:47800 (a ghost mirrors you)\r\n"
    "peer=127.0.0.1:47800\r\n"
    "; solo test only: how far to the side the mirrored ghost stands (metres)\r\n"
    "loopback_offset=3.0\r\n"
    "\r\n"
    "[player]\r\n"
    "; your name, shown to the other players\r\n"
    "name=Runner\r\n"
    "\r\n"
    "[ghost]\r\n"
    "; spawn other players' ghosts automatically when their packets arrive\r\n"
    "auto_spawn=1\r\n"
    "; how other players look: faith (your current outfit) or part of a model name,\r\n"
    "; e.g. RunVis (the red time-trial ghost). F8 cycles through the loaded models in game.\r\n"
    "model=faith\r\n"
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
    const std::wstring ini = d + L"mecmp.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            fputs(kDefaultIni, f);
            fclose(f);
            MLOG("config: wrote default %ls", ini.c_str());
        }
    }
    Settings& s = g_settings;
    s.netEnabled = readInt(ini, L"net", L"enabled", 1) != 0;
    s.port = static_cast<uint16_t>(readInt(ini, L"net", L"port", 47800));
    s.peer = readString(ini, L"net", L"peer", "");
    s.loopbackOffset = static_cast<float>(atof(readString(ini, L"net", L"loopback_offset", "3.0").c_str()));
    s.name = readString(ini, L"player", L"name", "Runner");
    if (s.name.empty()) s.name = "Runner";
    if (s.name.size() > 24) s.name.resize(24);
    s.autoSpawn = readInt(ini, L"ghost", L"auto_spawn", 1) != 0;
    s.model = readString(ini, L"ghost", L"model", "faith");
    s.verbose = readInt(ini, L"debug", L"verbose", 0) != 0;
    log::verbose = s.verbose;
    MLOG("config: name '%s', port %u, peer '%s', model '%s'%s", s.name.c_str(), s.port, s.peer.c_str(), s.model.c_str(),
         s.verbose ? ", verbose log" : "");
}

const Settings& get() { return g_settings; }
Settings& mut() { return g_settings; }

} // namespace mecmp::config
