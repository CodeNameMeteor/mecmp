#pragma once
// Minimal thread-safe logger: writes mecmp.log (see open() for where) and to the
// debugger output (view with Sysinternals DebugView).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mecmp::log {
// Opens mecmp.log next to `self` (mecmp.dll), else next to the exe, else %LOCALAPPDATA%\\MECMP.
void open(HMODULE self);
// Folder the log was written to (empty if none could be opened).
const wchar_t* directory();
void close();
// Detailed diagnostics (VLOG) are written only when mecmp.ini [debug] verbose=1.
extern bool verbose;
void write(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
} // namespace mecmp::log

#define MLOG(...) ::mecmp::log::write(__VA_ARGS__)
#define VLOG(...)                                                \
    do {                                                         \
        if (::mecmp::log::verbose) ::mecmp::log::write(__VA_ARGS__); \
    } while (0)
