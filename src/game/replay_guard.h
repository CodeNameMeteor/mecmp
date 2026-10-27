#pragma once
// Keeps the game's replay players from overrunning the heap when a recording with more bones than the last one
// is loaded on them. MECMP's full-body pose makes the player's runs 85-bone recordings (the game's own are 23); a
// time-trial ghost that is reused keeps the keyframe ring of its previous recording, and loading the 85-bone run
// into it wrote past the ring (crash racing your own ghost, 2026-10-27). Every replay player's ring is given room
// for 96 bones (replay_ring.h) on the thread that loads recordings into it, before the next load can happen.

namespace mecmp::game::replay_guard {

// Server game thread, every tick: the replay players of the characters of the ghost spawners (time-trial, echo
// and city ghosts).
void serverTick();
// Client thread, before the client replay updater runs (hooks.cpp): every client replay player in its list.
void beforeClientUpdate(void* updater);

} // namespace mecmp::game::replay_guard
