#pragma once
// Hooks. All of them swap a pointer in a vtable or an entity-creator table (data, not code):
//   - character spawner factories      -> level::onSpawnerCreated
//   - client replay player factory     -> ghosts / level reference setup
//   - client character factory         -> ghosts (visibility)
//   - ServerGameWorld::postUpdate      -> the per-tick callback (server game thread)

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

} // namespace mecmp::game::hooks
