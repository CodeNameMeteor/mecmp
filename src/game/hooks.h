#pragma once
// Hooks. All of them swap a pointer in a vtable or an entity-creator table (data, not code):
//   - PamUISystem update (vtable slot)  -> game menus do not pause the simulation in a session
//   - character spawner factories      -> level::onSpawnerCreated
//   - client replay player factory     -> ghosts / level reference setup
//   - client character factory         -> ghosts (visibility)
//   - ServerGameWorld::postUpdate      -> the per-tick callback (server game thread)
//   - client replay updater (vtable of the 2 updater objects) -> ghost poses, before the game reads them

#include <cstdint>

namespace mecmp::game::hooks {

using TickFn = void (*)(void* world, float dt);
struct Installed {
    bool tick = false;         // required
    bool spawners = false;     // optional: spawners are also found by a memory scan
    bool ghostClients = false; // client replay + client character: needed to show remote players
};
// Installs everything. Returns false if the tick hook could not be installed (nothing will work);
// `out` says which hooks are in.
bool install(TickFn onTick, Installed* out = nullptr);
// True once the client replay updater runs our pose writer (ghosts::beforeClientReplayUpdate) itself;
// until then poses are written from the render thread (ghosts::renderFrame).
bool clientUpdateHooked();
// True while one of the game's own menus asks for the game to be paused (pause menu, map, ...): the
// UI pause counts at PamUISystem +0x8c8 (+0x08 / +0x0c) are above 0. False before the UI system has
// run once or if the UI pause hook is not in. Any thread.
bool gameMenuOpen();

} // namespace mecmp::game::hooks
