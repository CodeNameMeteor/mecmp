#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mecmp::app::diagnostics {

// Logs access violations and similar faults (with module+offset and a rough backtrace) so a
// crash shows up in mecmp.log. Never handles the exception; the game's own handlers still run.
void install(HMODULE self);

} // namespace mecmp::app::diagnostics
