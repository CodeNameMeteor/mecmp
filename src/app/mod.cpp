#include "app/mod.h"

#include <atomic>
#include <string>

#include "app/diagnostics.h"
#include "core/config.h"
#include "core/log.h"
#include "core/typeinfo.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/hooks.h"
#include "game/level.h"
#include "game/local_player.h"
#include "net/net.h"
#include "ui/overlay.h"

namespace mecmp::app {
namespace {
std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_ticks{0};
uint64_t g_levelStartTick = 0;

// Server game thread, once per simulation tick.
void onTick(void* world, float dt) {
    static void* lastWorld = nullptr;
    if (world != lastWorld) { // new level: everything of the old one is gone
        if (lastWorld) game::ghosts::levelChanged();
        lastWorld = world;
        g_levelStartTick = g_ticks.load();
    }
    const uint64_t n = ++g_ticks;
    if (n == 1) VLOG("tick: first server tick (dt %.4f)", dt);
    static net::PlayerState local; // large; game thread only
    if (game::local_player::capture(local)) net::publishLocal(local);
    game::ghosts::tick(n, n - g_levelStartTick);
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
    MLOG("startup: game build recognised (%zu types)", types.size());
    return true;
}

void monitorLoop() {
    uintptr_t lastWorld = 0;
    struct Key { int vk; bool down; void (*action)(); };
    Key keys[] = {
        {VK_F7, false, [] { game::ghosts::requestSpawnOrRefresh(); }},
        {VK_F8, false, [] { game::ghosts::requestNextModel(); }},
        {VK_F9, false, [] { MLOG("status: net %s | %s", net::describe().c_str(), game::ghosts::status().c_str()); }},
        {VK_F12, false, [] { game::ghosts::requestHideAll(); }},
    };
    while (g_running) {
        const uintptr_t world = game::level::serverWorld();
        if (world != lastWorld) {
            MLOG("level %s", world ? "started" : "ended");
            if (lastWorld) game::level::reset(); // spawners of the old level are stale
            lastWorld = world;
        }
        for (Key& k : keys) {
            const bool d = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
            if (d && !k.down && world) k.action();
            k.down = d;
        }
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
    Beep(1000, 100); // audible "mod active"
    if (!game::hooks::install(&onTick)) {
        MLOG("startup: could not hook the game tick; staying inactive");
        return;
    }
    ui::install();
    const auto& cfg = config::get();
    if (cfg.netEnabled) net::start(cfg.port, cfg.join, cfg.loopbackOffset, cfg.name);
    else MLOG("net: disabled in mecmp.ini");
    MLOG("ready. %s F7 = spawn/show ghosts, F8 = next ghost model, F12 = hide ghosts, F9 = status (and join code).",
         cfg.join.empty() ? "Hosting: friends join with your join code (see below)." : "Joining: other players appear once the host answers.");
    monitorLoop();
}

void shutdown() {
    g_running = false;
    log::close();
}

} // namespace mecmp::app
