#pragma once
// mecmp.ini next to mecmp.dll. Created with commented defaults on first run.
//
// Threads (C-1): get() hands out a copy of the settings, taken under a short lock, so no thread
// ever reads a setting while another writes it. Changes are queued with change() from any thread
// (menu, game) and applied in order by the mod's own thread (applyPending), which also writes
// mecmp.ini, so file writes never happen on the render or game thread.

#include <cstdint>
#include <functional>
#include <string>

namespace mecmp::config {

struct Settings {
    // [net]
    bool netEnabled = true;
    uint16_t port = 47800;
    std::string join;                     // join code or ip:port of the host; empty = host a session
    float loopbackOffset = 3.0f;          // solo test: metres to the side the mirrored ghost stands
    // [player]
    std::string name = "Runner";
    // [ghost]
    bool autoSpawn = true;
    std::string model = "faith";          // "faith" or part of a model name (F8 cycles in game)
    // [ui]
    std::string menuKey = "INSERT";       // INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, PAUSE or F1..F12
    std::string menuPad = "LS+RS";        // gamepad buttons held together, e.g. LS+RS or BACK+Y; empty = none
    // [debug]
    bool verbose = false;
    bool ignoreBuildCheck = false;        // run even if the game build check fails (for testing a new build)
};

// Reads (and creates if missing) mecmp.ini in `dir`. Call once at startup.
void load(const std::wstring& dir);
Settings get();            // a snapshot (copy); cheap enough to call per tick
const std::wstring& dir(); // folder of mecmp.dll / mecmp.ini (with trailing backslash)
// Queues a change (any thread). `persist`: also write the user-editable settings to mecmp.ini.
void change(std::function<void(Settings&)> edit, bool persist = true);
// Mod thread: applies queued changes in order and saves once if any asked for it.
void applyPending();

} // namespace mecmp::config
