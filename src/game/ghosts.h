#pragma once
// Remote players as ghosts.
//
// Each remote player gets a ProjectionCharacter ("time-trial ghost") spawned from the level's
// city-ghost spawner. Every server tick the player's streamed pose is written into the ghost's
// server replay component (replicated to the client like a real replay), the ghost's client side
// is kept visible and placed at the player's position, and its look comes from models.h.
//
// Threads: tick/spawn/apply run on the server game thread; the on* hooks run on client threads.

#include <cstdint>
#include <string>
#include <vector>

namespace mecmp::game::ghosts {

// Hook callbacks. onClientReplayComp returns true if the component belongs to one of our ghosts.
bool onClientReplayComp(void* comp);
void onClientCharacter(void* ent);
// Startup: runtime TypeInfo of PamCharacterEntityData (0 if unknown), used to validate the ghost
// blueprint's character data before the first spawn.
void setCharacterDataType(uintptr_t runtimeTypeInfo);
// Startup: false when the client hooks are missing. Ghosts are then never spawned (they would sit
// at the replay's position, unanimated); the local player is still sent to the others.
void setEnabled(bool enabled);

// Render thread, every frame: writes each shown ghost's pose, interpolated for this moment, into its
// client side. The server tick runs at 30 Hz; without this the ghost would only move 30 times a
// second, which judders on faster displays.
void renderFrame();
// The same, called by the client replay updater just before it reads the poses (hooks.cpp): the
// game then never reads a pose we are halfway through writing.
void beforeClientReplayUpdate();

// Server tick: spawn ghosts for new players, drive all ghosts, keep their client side in shape.
void tick(uint64_t tick, uint64_t ticksInLevel);
void levelChanged(); // forget ghosts of the old level

// Requests (executed on the next tick).
void requestSpawnOrRefresh(); // F7: spawn missing ghosts now / re-apply the look; resumes auto spawning
void requestHideAll();        // F12: hide all ghosts and pause auto spawning
// Looks: each ghost shows its player's own look (sent with their name). Changes are picked up by the
// tick; nothing to request.

std::string status();
// Players whose ghost is in the level and shown now (for name tags). Any thread.
std::vector<uint32_t> shownPeers();

} // namespace mecmp::game::ghosts
