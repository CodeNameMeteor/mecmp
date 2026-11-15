# Handover: GridNode implementation plan branch

Branch `gn/implementation-plan` (PR #118) carries the work from `MECMP-implementation-plan-2026-11-15.md` (GridNode research
against main 7195f45). **Nothing on it has been compiled in this container (no compiler) or tested in game except where
`docs/progress.md` says the user tested it.** Build and test: see README; `docs/progress.md` has the detail per item.

## State
- Done and tested by the user (logs 2026-11-15): the player-spawned trigger (3.1, via ServerPlayerChangedCharacterMessage),
  ghosts re-shown after the game hides them on map close, settle delay removed.
- Done, not tested: 1.5, 2.1, 2.2 (incl. pin on the client thread), 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 2.10 (`g_placed`), 3.5.
- Checked in Ghidra, no code change: 0.4, 1.2, 1.3, 3.2; 0.6 / 3.6 (attachment flag found, hair not confirmed).
- Not done: 3.6, 3.3, 3.4, 1.4, 1.1, 2.10 look-apply time, 2.6 cache across levels.

## Things to know
- Every game address is in `src/game/engine.h`. A function the mod calls directly gets its first bytes checked in
  `src/game/fingerprint.cpp` (bytes from the dump); a mismatch turns that feature off or refuses the build.
- Poses are written on the client replay updater hook (`beforeClientReplayUpdate`), never the render thread.
- The game hides ghost client characters ~0.25 s after OnHideMap (`PamClientCharacterEntity::vf2`, property 0x35bc69ab);
  the setVisible hook notices and the next tick shows the ghost again.
- Parking (`kParkDepth`) hides hair that `setVisible(false)` leaves drawn; it stays until 3.6 is verified.
- Untested items worth a log: 2.1 over a long session, 2.7 after an echo look and a level change, time trial with a ghost near
  a player ghost, the cooldown path (2.8), 2.4, 2.9 with an adopted city ghost.

## Where the evidence is
GridNode research scratch (blocks A, L) is summarised in `docs/progress.md`; the plan itself is not in the repo.
