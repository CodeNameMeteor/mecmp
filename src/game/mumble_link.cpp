#include "game/mumble_link.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "game/camera.h"
#include "net/net.h"

namespace mecmp::game::mumble {
namespace {
// Mumble's Link structure (https://www.mumble.info/documentation/developer/positional-audio/link-plugin/).
struct LinkedMem {
    uint32_t uiVersion;
    uint32_t uiTick;
    float fAvatarPosition[3];
    float fAvatarFront[3];
    float fAvatarTop[3];
    wchar_t name[256];
    float fCameraPosition[3];
    float fCameraFront[3];
    float fCameraTop[3];
    wchar_t identity[256];
    uint32_t context_len;
    unsigned char context[256];
    wchar_t description[2048];
};
static_assert(sizeof(wchar_t) == 2, "Mumble expects UTF-16");

HANDLE g_map = nullptr;
LinkedMem* g_lm = nullptr;
uint32_t g_key = 0;      // session written last
std::string g_name;      // player name written last
bool g_openedExisting = false; // Mumble (its Link plugin) had made the memory before us
DWORD g_lastSent = 0;
bool g_loggedSending = false;
char g_context[32] = {};

bool mumbleProcessRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"mumble.exe") == 0;
    CloseHandle(snap);
    return found;
}

bool openLink() {
    if (g_lm) return true;
    g_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"MumbleLink");
    g_openedExisting = g_map != nullptr;
    if (!g_map) g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LinkedMem), L"MumbleLink");
    if (!g_map) return false;
    g_lm = static_cast<LinkedMem*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkedMem)));
    if (!g_lm) {
        CloseHandle(g_map);
        g_map = nullptr;
        return false;
    }
    g_key = 0;
    g_name.clear();
    MLOG("voice: Mumble link ready (%s)", g_openedExisting ? "Mumble's Link plugin is running"
                                                             : "Mumble not seen yet; it picks the game up when it starts");
    return true;
}

void closeLink() {
    if (!g_lm) return;
    g_lm->uiVersion = 0; // tells Mumble the game is gone
    UnmapViewOfFile(g_lm);
    CloseHandle(g_map);
    g_lm = nullptr;
    g_map = nullptr;
    g_lastSent = 0;
    g_loggedSending = false;
    MLOG("voice: Mumble link off");
}

void toMumble(const float* in, float* out) { // right-handed (Frostbite) -> left-handed (Mumble)
    out[0] = in[0];
    out[1] = in[1];
    out[2] = -in[2];
}

void writeWide(wchar_t* dst, size_t cap, const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dst, static_cast<int>(cap));
    if (n <= 0) dst[0] = 0;
    dst[cap - 1] = 0;
}
} // namespace

void update() {
    if (!config::get()->mumbleLink) {
        closeLink();
        return;
    }
    if (!openLink()) return;
    CameraView cam;
    if (!camera::read(cam)) return; // menus and loading: no tick, so Mumble stops positioning us
    if (g_lm->uiVersion != 2) {
        g_lm->uiVersion = 2;
        writeWide(g_lm->name, 256, "Mirror's Edge Catalyst (MECMP)");
        writeWide(g_lm->description, 2048, "MECMP: positional voice between players in the same session");
    }
    // The camera looks along minus its forward row (see camera_math.h).
    const float look[3] = {-cam.forward[0], -cam.forward[1], -cam.forward[2]};
    float pos[3], front[3], top[3];
    toMumble(cam.pos, pos);
    toMumble(look, front);
    toMumble(cam.up, top);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || !std::isfinite(front[i]) || !std::isfinite(top[i])) return;
    memcpy(g_lm->fAvatarPosition, pos, sizeof(pos));
    memcpy(g_lm->fAvatarFront, front, sizeof(front));
    memcpy(g_lm->fAvatarTop, top, sizeof(top));
    memcpy(g_lm->fCameraPosition, pos, sizeof(pos));
    memcpy(g_lm->fCameraFront, front, sizeof(front));
    memcpy(g_lm->fCameraTop, top, sizeof(top));

    // Context: who can hear whom positionally. Identity: shown by Mumble (the in-game name).
    const uint32_t key = net::sessionKey();
    const std::string name = config::get()->name;
    if (key != g_key) {
        g_key = key;
        char ctx[32] = {};
        const int n = key ? snprintf(ctx, sizeof(ctx), "MECMP-%08x", key) : 0;
        memset(g_lm->context, 0, sizeof(g_lm->context));
        memcpy(g_lm->context, ctx, static_cast<size_t>(n > 0 ? n : 0));
        memcpy(g_context, ctx, sizeof(g_context));
        g_lm->context_len = static_cast<uint32_t>(n > 0 ? n : 0);
        MLOG("voice: Mumble context %s", key ? ctx : "(none: not in a session yet)");
    }
    if (name != g_name) {
        g_name = name;
        writeWide(g_lm->identity, 256, name);
    }
    ++g_lm->uiTick;
    g_lastSent = GetTickCount();
    if (!g_loggedSending) {
        g_loggedSending = true;
        MLOG("voice: sending your position to Mumble (context %s)", g_context[0] ? g_context : "none: not in a session");
    }
}

Status status() {
    Status s;
    s.enabled = config::get()->mumbleLink;
    static DWORD checkedAt = 0;
    static bool running = false;
    const DWORD now = GetTickCount();
    if (!checkedAt || now - checkedAt > 3000) {
        checkedAt = now;
        running = mumbleProcessRunning();
    }
    s.mumbleFound = running || (g_lm && g_openedExisting);
    s.sending = g_lm && g_lastSent && now - g_lastSent < 1000;
    s.inSession = g_context[0] != 0;
    memcpy(s.context, g_context, sizeof(s.context));
    return s;
}

} // namespace mecmp::game::mumble
