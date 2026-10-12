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
#include "game/motion_stats.h"
#include "net/net.h"
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
    int echoTries = 0;
    DWORD nextEchoAt = 20000;
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
            echoTries = 0;
            nextEchoAt = 20000;
        }
        // The ghost look list (styles x colours) comes from the level's time-trial markers: look for it
        // 20 s into a level, and again after 1 and 3 minutes if there was none yet.
        if (world && echoTries < 3 && GetTickCount() - levelSince > nextEchoAt && game::models::echoes()->empty()) {
            ++echoTries;
            if (game::models::findEchoes()) fixMyLook();
            nextEchoAt = 20000 + 60000u * ((1u << echoTries) - 1);
        }
        // Loaded after the level was created (or the game re-used spawners we never saw being
        // made)? Look for them in memory: 15 s into the level, and if something is still missing
        // again after 1, 3 and 7 minutes. A full scan reads the whole private heap, so no more.
        constexpr int kMaxScansPerLevel = 4;
        if (world && scans < kMaxScansPerLevel && GetTickCount() - levelSince > nextScanAt && !game::level::complete()) {
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
    Beep(1000, 100); // audible "mod active"
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
    });
    ui::install();
    const auto cfg = config::get();
    net::setLook(config::lookHash(cfg->look));
    if (!cfg->netEnabled) MLOG("net: disabled in mecmp.ini");
    else if (!net::start(cfg->port, cfg->join, cfg->loopbackOffset, cfg->name))
        MLOG("net: multiplayer is NOT running (see the net: lines above); the rest of the mod still works");
    MLOG("ready. %s Menu: %s%s%s.",
         cfg->join.empty() ? "Hosting: friends join with your join code (see below)." : "Joining: other players appear once the host answers.",
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
