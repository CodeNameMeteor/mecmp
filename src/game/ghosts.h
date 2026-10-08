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

namespace mecmp::game::ghosts {

// Hook callbacks. onClientReplayComp returns true if the component belongs to one of our ghosts.
bool onClientReplayComp(void* comp);
void onClientCharacter(void* ent);

// Server tick: spawn ghosts for new players, drive all ghosts, keep their client side in shape.
void tick(uint64_t tick, uint64_t ticksInLevel);
void levelChanged(); // forget ghosts of the old level

// Requests (executed on the next tick).
void requestSpawnOrRefresh(); // F7: spawn missing ghosts now / re-apply the look; resumes auto spawning
void requestHideAll();        // F12: hide all ghosts and pause auto spawning
void requestNextModel();      // F8
void requestModel(const std::string& name); // menu: use this model for everyone's ghost

std::string status();

} // namespace mecmp::game::ghosts
