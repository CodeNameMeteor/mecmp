#pragma once
// Top level: startup, the per-tick update and the hotkey/monitor loop.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mecmp::app {

constexpr const char* kVersion = "0.3.0";

// Runs on the mod's own thread until the game exits.
void run(HMODULE self);
void shutdown();

} // namespace mecmp::app
