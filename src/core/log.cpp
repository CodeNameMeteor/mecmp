#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "log.h"

namespace mecmp::log {
bool verbose = false;
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
// mecmp.log stops growing at this size (S-9); the debugger output keeps everything.
constexpr size_t kMaxLogBytes = 16u << 20;
size_t g_written = 0;
bool g_full = false;
}

namespace {
// Directory of `module` (or of the exe if null), with a trailing backslash.
bool moduleDir(HMODULE module, wchar_t* path) {
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    for (DWORD i = n; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') { path[i] = 0; return true; }
    }
    return false;
}

FILE* tryOpen(const wchar_t* dir) {
    wchar_t path[MAX_PATH];
    // Longest name below is "mecmp.prev.log" (14 characters) plus the terminating NUL.
    if (lstrlenW(dir) + 15 >= MAX_PATH) return nullptr;
    lstrcpyW(path, dir);
    lstrcatW(path, L"mecmp.log");
    // Keep the previous run's log as mecmp.prev.log (a crash run must not erase the one before it).
    wchar_t prev[MAX_PATH];
    lstrcpyW(prev, dir);
    lstrcatW(prev, L"mecmp.prev.log");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
    FILE* f = _wfopen(path, L"w");
    if (f) {
        // Remember where it went so we can tell the user (also visible in DebugView).
        OutputDebugStringW(L"[MECMP] log file: ");
        OutputDebugStringW(path);
        OutputDebugStringW(L"\n");
    }
    return f;
}
} // namespace

std::wstring g_path;

void open(HMODULE self) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    // 1) next to mecmp.dll (works when injected from a dev folder),
    // 2) next to the game exe, 3) %LOCALAPPDATA%\MECMP (always writable).
    wchar_t dir[MAX_PATH];
    if (moduleDir(self, dir) && (g_file = tryOpen(dir))) { g_path = dir; return; }
    if (moduleDir(nullptr, dir) && (g_file = tryOpen(dir))) { g_path = dir; return; }
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH - 16);
    if (n > 0 && n < MAX_PATH - 16) {
        lstrcatW(dir, L"\\MECMP\\");
        CreateDirectoryW(dir, nullptr);
        if ((g_file = tryOpen(dir))) g_path = dir;
    }
}

const wchar_t* directory() { return g_path.c_str(); }

void close() {
    // Called at process exit, where a thread that held the lock may already be gone: never wait.
    if (!g_mutex.try_lock()) return;
    if (g_file) { fclose(g_file); g_file = nullptr; }
    g_mutex.unlock();
}

void writeUrgent(const char* line) {
    OutputDebugStringA("[MECMP] ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    if (!g_mutex.try_lock()) return; // the log is busy (maybe on this very thread): debugger output only
    if (g_file && !g_full) {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    g_mutex.unlock();
}

void write(const char* fmt, ...) {
    char msg[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(msg, sizeof(msg), "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg + n, sizeof(msg) - n - 2, fmt, ap);
    va_end(ap);
    size_t len = strlen(msg);
    msg[len] = '\n';
    msg[len + 1] = 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    OutputDebugStringA("[MECMP] ");
    OutputDebugStringA(msg);
    if (g_file && !g_full) {
        if (g_written + len + 1 > kMaxLogBytes) {
            fputs("[log] mecmp.log reached 16 MB; nothing more is written to it this session\n", g_file);
            g_full = true;
        } else {
            fputs(msg, g_file);
            g_written += len + 1;
        }
        fflush(g_file);
    }
}
} // namespace mecmp::log
