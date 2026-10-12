#pragma once
// mecmp.ini next to mecmp.dll. Created with commented defaults on first run.
//
// Threads (C-1): get() hands out a copy of the settings, taken under a short lock, so no thread
// ever reads a setting while another writes it. Changes are queued with change() from any thread
// (menu, game) and applied in order by the mod's own thread (applyPending), which also writes
// mecmp.ini, so file writes never happen on the render or game thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mecmp::config {

struct Settings {
    // [net]
    bool netEnabled = true;
    uint16_t port = 47800;
    std::string join;                     // join code or ip:port of the host; empty = host a session
    float loopbackOffset = 3.0f;          // solo test: metres along world X the mirrored ghost stands
    // [player]
    std::string name = "Runner";
    std::string look = "faith";           // how you look to others: faith, or echo:<hash> (a ghost style + colour)
    // [ghost]
    bool autoSpawn = true;
    // [ui]
    std::string menuKey = "INSERT";       // INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, PAUSE or F1..F12
    std::string menuPad = "LS+RS";        // gamepad buttons held together, e.g. LS+RS or BACK+Y; empty = none
    bool showNames = true;                // names above other players' ghosts
    bool showNotices = true;              // "<name> joined / left" at the top of the screen
    bool showChat = true;                 // chat messages bottom-left
    std::string chatKey = "T";            // opens the chat line: a letter, ENTER, or a menu key name
    // [debug]
    bool verbose = false;
    bool ignoreBuildCheck = false;        // run even if the game build check fails (for testing a new build)
};

// Reads (and creates if missing) mecmp.ini in `dir`. Call once at startup.
void load(const std::wstring& dir);
// The current settings. The snapshot never changes: copying the pointer is cheap enough for every tick.
std::shared_ptr<const Settings> get();
const std::wstring& dir(); // folder of mecmp.dll / mecmp.ini (with trailing backslash)
// Queues a change (any thread). `persist`: also write the user-editable settings to mecmp.ini.
void change(std::function<void(Settings&)> edit, bool persist = true);
// Mod thread: applies queued changes in order and saves once if any asked for it.
void applyPending();

// "faith" -> 0, "echo:1a2b3c4d" -> 0x1a2b3c4d: the look sent to other players.
uint32_t lookHash(const std::string& look);
std::string lookName(uint32_t hash);

} // namespace mecmp::config
