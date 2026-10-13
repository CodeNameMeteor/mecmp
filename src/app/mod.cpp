#include "app/mod.h"

#include <atomic>
#include <mutex>
#include <string>

#include "app/diagnostics.h"
#include "core/config.h"
#include "core/log.h"
#include "core/typeinfo.h"
#include "game/camera_finder.h"
#include "game/engine.h"
#include "game/fingerprint.h"
#include "game/ghosts.h"
#include "game/teleport.h"
#include "game/hooks.h"
#include "game/level.h"
#include "game/local_player.h"
#include "game/models.h"
#include "game/ping_point.h"
#include "game/motion_stats.h"
#include "net/net.h"
#include "net/serverlist.h"
#include "ui/gamepad.h"
#include "ui/overlay.h"

namespace mecmp::app {
namespace {
std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_ticks{0};
std::mutex g_localMotionM;
game::MotionStats g_localMotion; // what this player captures, tick to tick (F9)
uint64_t g_levelStartTick = 0;

// Server game thread, once per simulation tick.
void onTick(void* world, float dt) {
    static void* lastWorld = nullptr;
    if (world != lastWorld) { // new level: everything of the old one is gone
        if (lastWorld) { game::ghosts::levelChanged(); game::teleport::levelChanged(); }
        lastWorld = world;
        g_levelStartTick = g_ticks.load();
    }
    const uint64_t n = ++g_ticks;
    if (n == 1) VLOG("tick: first server tick (dt %.4f)", dt);
    static net::PlayerState local; // large; game thread only
    if (game::local_player::capture(local)) {
        net::publishLocal(local);
        std::lock_guard<std::mutex> lock(g_localMotionM);
        g_localMotion.add(local);
    }
    game::ghosts::tick(n, n - g_levelStartTick);
    game::teleport::tick();
    game::ping::tick();
}

// Waits until the engine has built its type list, then checks that this is the game build the
// mod was made for (by comparing a few TypeInfo addresses).
bool gameMatches() {
    const DWORD start = GetTickCount();
    size_t last = 0;
    int stable = 0;
    while (g_running && GetTickCount() - start < 5 * 60 * 1000) {
        const size_t n = TypeList::countFrom(game::rva(game::kTypeInfoListHead));
        stable = (n >= 9000 && n == last) ? stable + 1 : 0;
        if (stable >= 4) break;
        last = n;
        Sleep(250);
    }
    if (last < 9000) {
        MLOG("startup: the engine's type list never filled; unsupported game version?");
        return false;
    }
    TypeList types;
    types.load(game::rva(game::kTypeInfoListHead));
    struct Check { const char* name; uintptr_t va; };
    const Check checks[] = {{"PamReplayPlayerComponentData", game::TI_PamReplayPlayerComponentData},
                            {"ServerCharacterEntity", game::TI_ServerCharacterEntity},
                            {"ObjectBlueprint", game::TI_ObjectBlueprint}};
    for (const Check& c : checks) {
        const TypeEntry* t = types.find(c.name);
        if (!t || t->ti != game::rva(c.va)) {
            MLOG("startup: %s is not where this build expects it; this game version is not supported", c.name);
            return false;
        }
    }
    if (const TypeEntry* t = types.find("PamCharacterEntityData")) game::ghosts::setCharacterDataType(t->ti);
    else MLOG("startup: PamCharacterEntityData not in the type list");
    MLOG("startup: game build recognised (%llu types)", static_cast<unsigned long long>(types.size()));
    return true;
}

// "Ghost" picked before the look list was known (hash ffffffff): take the first look now.
void fixMyLook() {
    if (config::lookHash(config::get()->look) != 0xffffffffu) return;
    const uint32_t h = game::models::defaultEcho();
    if (!h) return;
    net::setLook(h);
    const std::string look = config::lookName(h);
    config::change([look](config::Settings& c) { c.look = look; });
}

void monitorLoop() {
    uintptr_t lastWorld = 0;
    DWORD levelSince = GetTickCount(), nextScanAt = 15000;
    int scans = 0;
    int echoScans = 0;
    DWORD echoCheckedAt = 0;
    int worldsSeen = 0;
#if MECMP_DEBUG_KEYS
    // Developer hotkeys (not in release builds; see CMakeLists.txt).
    struct Key { int vk; bool down; void (*action)(); };
    Key keys[] = {
        {VK_F7, false, [] { game::ghosts::requestSpawnOrRefresh(); }},
        {VK_F9, false, [] {
             std::string local;
             {
                 std::lock_guard<std::mutex> lock(g_localMotionM);
                 local = g_localMotion.report();
             }
             MLOG("status: net %s | %s | local capture %s", net::describe().c_str(), game::ghosts::status().c_str(), local.c_str());
         }},
        {VK_F12, false, [] { game::ghosts::requestHideAll(); }},
        {VK_F10, false, [] { game::camera_finder::run(); }},
    };
#endif
    while (g_running) {
        const uintptr_t world = game::level::serverWorld();
        if (world != lastWorld) {
            MLOG("level %s", world ? "started" : "ended");
            if (lastWorld) game::level::reset(); // spawners of the old level are stale
            lastWorld = world;
            levelSince = GetTickCount();
            nextScanAt = 15000;
            scans = 0;
            echoScans = 0;
            if (world) ++worldsSeen;
        }
        // Memory scans (each reads the game's whole heap, about a second) are a fallback, mainly for the
        // level that was already running when the mod loaded; later levels are seen being built by
        // the hooks (spawners, client objects, the look catalogue's time-trial markers), so they get
        // at most one scan, 60 s in, and only if something is still missing in a level you play in.
        const bool firstWorld = worldsSeen == 1;
        const DWORD inLevel = GetTickCount() - levelSince;
        const bool lateScanDue = !firstWorld && inLevel > 60000 && game::level::playerCharacter();
        // The ghost look list (styles x colours) comes from the level's time-trial markers, noted as they
        // are created: checked every second (cheap). Fallback memory scan 20 s and 60 s into the first
        // level. Ghosts waiting for the list get their look right after.
        if (world && GetTickCount() - echoCheckedAt >= 1000 && game::models::echoes()->empty()) {
            echoCheckedAt = GetTickCount();
            static constexpr DWORD kEchoScanAt[] = {20000, 60000};
            const bool scanNow = firstWorld ? echoScans < 2 && inLevel > kEchoScanAt[echoScans] : echoScans < 1 && lateScanDue;
            if (scanNow) ++echoScans;
            if (game::models::findEchoes(scanNow)) {
                fixMyLook();
                game::ghosts::lookListArrived();
            }
        }
        // Loaded after the level was created (or the game re-used spawners we never saw being
        // made)? Look for them in memory: 15 s into the level, and if something is still missing
        // again after 1, 3 and 7 minutes. A full scan reads the whole private heap, so no more.
        constexpr int kMaxScansPerLevel = 4;
        if (world && (firstWorld ? scans < kMaxScansPerLevel && inLevel > nextScanAt : scans < 1 && lateScanDue) &&
            !game::level::complete()) {
            ++scans;
            game::level::scanForMissing();
            nextScanAt = 15000 + 60000u * ((1u << scans) - 1); // 75 s, 195 s, 435 s after the level started
            if (scans == kMaxScansPerLevel && !game::level::complete())
                MLOG("level: still missing spawners after %d scans; not scanning this level again", scans);
        }
        config::applyPending(); // menu and game changes, in order (and mecmp.ini)
        ui::gamepad::maintain();  // once the game has loaded XInput
#if MECMP_DEBUG_KEYS
        for (Key& k : keys) {
            const bool d = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
            if (d && !k.down && world) k.action();
            k.down = d;
        }
#endif
        Sleep(50);
    }
}
} // namespace

void run(HMODULE self) {
    log::open(self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    MLOG("MECMP %s loaded from %ls", kVersion, path);
    diagnostics::install(self);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    config::load(dir);

    if (!gameMatches()) {
        MLOG("startup: staying inactive");
        return;
    }
    if (!game::fingerprint::check()) {
        if (!config::get()->ignoreBuildCheck) {
            MLOG("startup: this game build differs from the one MECMP was made for; staying inactive "
                 "([debug] ignore_build_check=1 in mecmp.ini overrides this, at your own risk)");
            return;
        }
        MLOG("startup: build check failed but ignore_build_check=1: continuing");
    }
    game::hooks::Installed hooks;
    if (!game::hooks::install(&onTick, &hooks)) {
        MLOG("startup: could not hook the game tick; staying inactive");
        return;
    }
    game::ghosts::setEnabled(hooks.ghostClients);
    // The game is exiting: stop sending and close the router port while the network still works.
    ui::setOnWindowDestroyed([] {
        MLOG("game window closed: stopping multiplayer");
        net::shutdown(2000);
        net::servers::shutdown();
    });
    ui::install();
    const auto cfg = config::get();
    net::setLook(config::lookHash(cfg->look));
    const bool startNow = cfg->netEnabled && cfg->autoStart;
    if (!cfg->netEnabled) MLOG("net: disabled in mecmp.ini");
    else if (!cfg->autoStart) MLOG("net: offline until you host or join in the menu ([net] auto_start=1 starts at launch)");
    else if (!net::start(cfg->port, cfg->join, cfg->loopbackOffset, cfg->name))
        MLOG("net: multiplayer is NOT running (see the net: lines above); the rest of the mod still works");
    MLOG("ready. %s Menu: %s%s%s.",
         !startNow ? "Host, join or pick a server in the menu."
         : cfg->join.empty() ? "Hosting: friends join with your join code (see below)."
                             : "Joining: other players appear once the host answers.",
         cfg->menuKey.c_str(), cfg->menuPad.empty() ? "" : " or ", cfg->menuPad.c_str());
#if MECMP_DEBUG_KEYS
    MLOG("developer keys: F7 = spawn/show ghosts, F12 = hide ghosts, F9 = status, "
         "F10 = find the camera matrix");
#endif
    monitorLoop();
}

// Process exit (DllMain, loader lock held): no joins or network here. Multiplayer was already
// stopped when the game window closed; the DLL is pinned, so this is never a mid-game unload.
void shutdown() {
    g_running = false;
    log::close();
}

} // namespace mecmp::app
