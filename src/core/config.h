#pragma once
// mecmp.ini next to mecmp.dll. Created with commented defaults on first run.
//
// Threads (C-1): get() hands out a copy of the settings, taken under a short lock, so no thread
// ever reads a setting while another writes it. Changes are queued with change() from any thread
// (menu, game) and applied in order by the mod's own thread (applyPending), which also writes
// mecmp.ini, so file writes never happen on the render or game thread.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mecmp::config {

// The marker wheels (hold the marker key): wheels of nine marker types each. Three come ready-made,
// and two more can be added in the menu. A slot's title and icon are yours, so every player's wheels
// can say something different.
constexpr int kPingSlots = 9;
constexpr int kWheels = 5;        // at most
constexpr int kDefaultWheels = 3; // the ready-made ones; these cannot be removed
// The icons a marker can have (drawn by ui/glyph.cpp). The names are what mecmp.ini says.
constexpr int kPingGlyphs = 33; // PING can carry 64
const char* glyphName(uint8_t glyph);                             // "diamond" for anything unknown
uint8_t glyphFromName(const std::string& name, uint8_t fallback); // `fallback` for an unknown name
struct WheelSlot {
    std::string label; // at most net::protocol::kMaxPingLabelBytes; empty = a marker without a title
    uint8_t glyph = 0;
};
using Wheel = std::array<WheelSlot, kPingSlots>;
Wheel defaultWheel(int wheel); // the ready-made wheels; an empty wheel for the others

// A saved spot: where a marker was, with its icon and title. Placing a bookmark puts a marker there again.
constexpr size_t kMaxBookmarks = 20;
struct Bookmark {
    std::string label;
    uint8_t glyph = 0;
    float pos[3] = {0, 0, 0};
};
std::array<Wheel, kWheels> defaultWheels();

struct Settings {
    // [net]
    bool netEnabled = true;
    uint16_t port = 47800;
    std::string join;                     // join code or ip:port of the host; empty = host a session
    bool autoStart = false;               // start that session when the game starts (else wait for the menu)
    float loopbackOffset = 3.0f;          // solo test: metres along world X the mirrored ghost stands
    std::string servers;                  // the menu's server list: addresses, comma separated
    // [player]
    std::string name = "Runner";
    std::string look = "faith";           // how you look to others: faith, or echo:<hash> (a ghost style + colour)
    std::string pingColor = "ffd23c";     // colour of your markers for everyone, RRGGBB
    std::string pingAccent = "ffffff";    // colour of the detail inside a marker (the diamond's inner diamond)
    std::array<Wheel, kWheels> wheels = defaultWheels();
    int wheelCount = kDefaultWheels;      // how many of them exist: kDefaultWheels..kWheels
    // [bookmarks]
    std::vector<Bookmark> bookmarks;      // at most kMaxBookmarks
    int pingSeconds = 30;                 // how long your markers stay: 5..300
    int pingMax = 3;                      // how many markers you can have at once: 1..5
    // [ghost]
    bool autoSpawn = true;
    // [ui]
    std::string menuKey = "INSERT";       // INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, PAUSE or F1..F12
    std::string menuPad = "LS+RS";        // gamepad buttons held together, e.g. LS+RS or BACK+Y; empty = none
    bool showNames = true;                // names above other players' ghosts
    bool showPings = true;                // markers placed by players
    bool showArrows = true;               // arrows on the screen's edge to players who are off screen
    bool pingPulse = true;                // a new marker flashes briefly when it arrives
    bool pingAim = true;                  // a marker goes where you look (false: where you stand)
    std::string pingKey = "G";            // press = place a marker, hold = the wheel: a letter, ENTER, or a menu key name; empty = none
    std::string pingPad;                  // gamepad buttons for the same (as menu_pad); empty = none
    int pingWheel = 0;                    // the wheel that opens (the one used last)
    int pingLastWheel = 0, pingLastSlot = 0; // the marker picked last
    int pingTap = 0;                      // a short press places: 0 = the marker picked last, 1 = always the one below
    int pingFixedWheel = 0, pingFixedSlot = 0;
    // Removing all your markers: hold this key for a moment (in the open wheel the middle mouse button
    // does the same); on a gamepad, press these buttons while the marker buttons are held.
    std::string pingClearKey = "H";       // as ping_key, or MOUSE3 (middle), MOUSE4, MOUSE5; empty = none
    std::string pingClearPad = "RS";      // gamepad buttons (as menu_pad); empty = none
    // [voice]
    bool mumbleLink = true;               // positional voice through Mumble's Link plugin
    bool showNotices = true;              // "<name> joined / left / placed a marker" at the top of the screen
    bool showChat = true;                 // chat messages bottom-left
    std::string chatKey = "ENTER";        // opens the chat line: a letter, ENTER, or a menu key name
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
