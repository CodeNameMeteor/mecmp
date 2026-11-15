# Handover: GridNode implementation plan branch

Branch `gn/implementation-plan` (PR #118) carries the work from `MECMP-implementation-plan-2026-11-15.md` (GridNode research
against main 7195f45). **Nothing on it has been compiled in this container (no compiler) or tested in game except where
`docs/progress.md` says the user tested it.** Build and test: see README; `docs/progress.md` has the detail per item.

## State
- Done and tested by the user (logs 2026-11-15): the player-spawned trigger (3.1, via ServerPlayerChangedCharacterMessage),
  ghosts re-shown after the game hides them on map close, settle delay removed.
- Done, not tested: 1.5, 2.1, 2.2 (incl. pin on the client thread), 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 2.10 (`g_placed`), 3.3, 3.5, and the 3.4 cleanup plus the registry check (log only).
- Checked in Ghidra, no code change: 0.4, 1.2, 1.3, 3.2; 0.6 / 3.6 (attachment flag found, hair not confirmed).
- Not done: 3.6, 3.4 (scans not replaced yet; see progress.md), 1.4, 1.1, 2.10 look-apply time, 2.6 cache across levels.

## Things to know
- Every game address is in `src/game/engine.h`. A function the mod calls directly gets its first bytes checked in
  `src/game/fingerprint.cpp` (bytes from the dump); a mismatch turns that feature off or refuses the build.
- Poses are written on the client replay updater hook (`beforeClientReplayUpdate`), never the render thread.
- The game hides ghost client characters ~0.25 s after OnHideMap (`PamClientCharacterEntity::vf2`, property 0x35bc69ab);
  the setVisible hook notices and the next tick shows the ghost again.
- Parking (`kParkDepth`) hides hair that `setVisible(false)` leaves drawn; it stays until 3.6 is verified.
- Everything not yet tested is in the checklist below.

## Test checklist (nothing below has been run in game)
Build the branch, play two players (host + one joiner, or the test server), keep `mecmp.log`, and send it back with the
item numbers. A line that must NOT appear is marked "never". Do them in one or two sessions; items overlap.

1. **Startup (all).** Log has "hook ghost setVisible log: installed" and "hook client spawner add: installed". Any
   "build check: ..." line (never, except on a changed game build) names the item to turn off.
2. **3.3 spawn hook.** Join, let the other player's ghost appear. Then open the map and look at "Your Echo". Expect: the echo
   never turns into the player's ghost and the ghost keeps moving. Log: never "list reordered by the safety net" (if it shows,
   the hook missed; send the log). "list reordered as a character was added" is fine (needs verbose logging).
3. **2.2 anchor pin.** Hide a ghost (player leaves, or F12) and show it again. Expect: the body drops out of sight and comes
   back; no ghost left standing where the player was; no flicker when it returns.
4. **2.9 server playback stop.** With a second player, a ghost that came from the game's city ghosts (not the mod's own
   spawner) should follow that player as before. Log: never "build check: ReplayPlayback_Stop". If a ghost freezes or the
   game crashes right after a ghost is first driven, send the log: that points at this change.
5. **2.4 / 2.5 bad pose.** Normal play only; if a ghost's client rejects a pose the ghost must be skipped, not teleported. Look for
   "rejects the pose" lines; a ghost that stands still while its player moves is the symptom to report.
6. **2.6 / 2.7 looks across levels.** Pick an Echo look or model for a ghost, change level (or enter and leave a time trial),
   pick again. Expect: the look applies each time, the game's own Echo/placeholder pieces look normal afterwards, no crash.
7. **2.8 spawn cooldown.** Join right after a level load, and again right after a ghost was removed. Expect: the ghost appears
   a moment later; the log shows a wait, not "spawner failed" counts that blacklist the spawner.
8. **2.1 long session.** 30 minutes with a ghost driven: memory (Task Manager) should stay flat, no growth per level change.
9. **Time trial.** Run a time trial with a player ghost near the game's own trial ghost: both show, neither takes the other's pose.
10. **Map close.** Open and close the map 10 times: ghosts must not vanish for more than a moment (known: the game hides them,
    MECMP shows them next tick).
11. **3.4 registry check.** In the mod menu turn on "Look like this in my own game too" and pick an Echo look (or the Prologue
    outfit); that makes the mod search for the live look pieces. Expect in the log: "look: N look pieces are loaded" and then
    a "registry check:" line. Send the line from at least two different levels (change level and pick a look again).
    "MATCH" means the game's data registry could replace the heap scan; "DIFFERENT" lines name what differs. "the object
    table is not readable" means the layout differs from the dump. Nothing else should change; if looks stop working
    after this build, send the log (the check reads memory only).

## Where the evidence is
GridNode research scratch (blocks A, L) is summarised in `docs/progress.md`; the plan itself is not in the repo.
