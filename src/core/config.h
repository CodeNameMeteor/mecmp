#pragma once
// mecmp.ini next to mecmp.dll. Created with commented defaults on first run.

#include <cstdint>
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
    std::string menuKey = "INSERT";       // INSERT, HOME, END, DELETE or F1..F12
    // [debug]
    bool verbose = false;
};

// Reads (and creates if missing) mecmp.ini in `dir`. Call once at startup.
void load(const std::wstring& dir);
const Settings& get();
const std::wstring& dir(); // folder of mecmp.dll / mecmp.ini (with trailing backslash)
Settings& mut();  // runtime changes (menu)
void save();      // writes name, join and model back to mecmp.ini

} // namespace mecmp::config
