#pragma once
// mecmp.ini next to mecmp.dll. Created with commented defaults on first run.

#include <cstdint>
#include <string>

namespace mecmp::config {

struct Settings {
    // [net]
    bool netEnabled = true;
    uint16_t port = 47800;
    std::string peer = "127.0.0.1:47800"; // ip:port of the other player; empty = wait to be contacted
    float loopbackOffset = 3.0f;          // solo test: metres to the side the mirrored ghost stands
    // [player]
    std::string name = "Runner";
    // [ghost]
    bool autoSpawn = true;
    std::string model = "faith";          // "faith" or part of a model name (F8 cycles in game)
    // [debug]
    bool verbose = false;
};

// Reads (and creates if missing) mecmp.ini in `dir`. Call once at startup.
void load(const std::wstring& dir);
const Settings& get();
Settings& mut(); // runtime changes (e.g. model picked in game); not written back yet

} // namespace mecmp::config
