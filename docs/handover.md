# Handover: GridNode implementation plan

Source: `MECMP-implementation-plan-2026-11-15.md` (GridNode research against main 7195f45). The plan itself is not in the repo.
**Nothing was compiled in the container that wrote this (no game toolchain). Items marked "tested" were run in game by the user.**
In-game results so far (2026-11-16, solo test + server run, 4 logs, see progress.md "First/Second/Third in-game log"): startup,
3.3 spawn hook, 2.2 pin, map close, level changes, checkpoint reload, time trials, look changes, outfits (Faith, prologue) and the
registry piece list all behaved. Not run: 2.1 30-minute memory test, 2.4/2.5 bad-pose case, 2.8 cooldown, own_spawner research.
Per-item detail and the Ghidra evidence: `docs/progress.md` (look for the item number). Open research questions for another
reader (Gemini): `docs/research-requests.md`.

## Where the work is
- **In `main` (PR #118, merged 2026-11-15):** the first batch. Items below marked [main].
- **On branch `gn/implementation-plan`, draft PR after #118:** everything marked [branch]. These commits are not in `main` yet.

## Status of every plan item
Tested = the user ran it in game and reviewed the log. Untested = written, not run.

| Item | Status | Why / what is left | Evidence |
|---|---|---|---|
| 0.1 pose buffers freed by the engine | confirmed | the destructor frees only the current pointers; null arena member = shared default arena | progress "Part 0", "Ghidra pass 2" |
| 0.2 count flag drops the pose | confirmed | `update` copies the pose only if +0x2e4 and +0x2e5 are set | "Ghidra pass 2" |
| 0.3 spawn cooldown | confirmed | refuses on +0xf0 > 0.01 and +0x108 bit 8 | "Part 0" |
| 0.4 bus id value | partly | formula true for the spawn bus; MECMP reads character +0x28, which object that is is not shown | "Part 0", "Ghidra pass 2" |
| 0.5 reference drop on look data | not supported | the add/release is on the spawn template's container, balanced; the apply path never touches +0x08 | "2.6" |
| 0.6 attachment flag | partly | flag at entry +8 found and rewritten every update; hair as an entry and the flag's writer not shown | "0.6 / 3.6", "Ghidra pass 2" |
| 0.7 slot 3 destroys a character | inconclusive | the destroyer found is `stopReplay` (unspawns the spawner's last character) | "Ghidra pass 2" |
| 0.8 `ReplaySetup_Build` arguments | readable | needs the rig table; more fields than the setup depend on the data | "Part 0", "Updaters" |
| 1.1 own spawner | no change | default off; a working one needs its own registered data (research); the lookup was read but equal-key order is not shown; `own_spawner=1` test decides | "Ghidra pass 2" |
| 1.2 game takes ghosts back | no change | mechanism confirmed (StartReplay adopts the last character, StopReplay unspawns it); the 3.1 trigger and keep-last logic already cover it in logs | "Ghidra pass 2" |
| 1.3 bus id claim | no change | the claim already uses a bus id only when unique; blocked on 0.4 | "0.4 / 1.3 / 3.2" |
| 1.4 shared data patched in place | not done | needs the rig table or a patch-around-create change; both need a ghost-spawn game test and carry crash risk | "Part 0", "Updaters" |
| 1.5 spawner slot comment | done [main] | comment only | |
| 2.1 pose buffer leak | done [main], untested | long-session test pending (checklist 8) | |
| 2.2 client fields off the server thread | done: flags and playback [main], anchor pin [branch]; untested | checklist 3 | |
| 2.3 render-thread fallback | done [main], untested | | |
| 2.4 pose into a rejecting client | done [main], untested | needs a client that rejects a pose to see it | |
| 2.5 failed pose read | done [main], untested | | |
| 2.6 look data memory | partly [branch], untested | header no longer copied; cache across levels not done (cached data points at the old level's assets) | "2.6" |
| 2.7 placeholder pieces restored | done [main], untested | checklist 6 | |
| 2.8 spawn cooldown | done [main], untested | checklist 7 | |
| 2.9 stop server playback | done [branch], untested | risk: see below; checklist 4 | "2.9" |
| 2.10 small ones | `g_placed` done [main]; look-apply left: it is the intended one-rebuild-per-tick throttle; the other bullets go with 1.1 / 3.4 | | |
| 3.1 lifecycle trigger | done [main], tested | ServerPlayerChangedCharacterMessage; the plan's message is never sent locally | "3.1 correction" |
| 3.2 claim window | no change | bus id unique-match already used | "0.4 / 1.3 / 3.2" |
| 3.3 act at spawner insertion | done [branch], untested | 250 ms pass kept as a logged safety net; the OnHideMap double refresh stays until a test shows the net never fires | "3.3" |
| 3.4 heap scans -> registry | partly [branch], **tested**: the look pieces the mod uses come from the registry (16 pieces, 0 ms, 3 levels), scan is the fallback | cleanup, registry reader, log-only comparison, look list from the registry; the piece scan and entity scans are NOT replaced (wait for MATCH logs; components not known to be in class lists) | "3.4 research", "3.4 additions" |
| 3.5 player character from the player manager | done [main], tested in practice | spawner path kept as fallback | |
| 3.6 parking -> attachment flag | not done | parking works today; hair-as-entry unproven; needs an in-game experiment | "0.6 / 3.6" |
| 3.7 UI system global | already in the code | the global is the source; the hook is a fallback that logs if they differ | "3.4 additions and 3.7" |
| Map-close vanish (not in the plan) | done [main], tested | the game hides ghosts ~0.25 s after OnHideMap; setVisible hook + next-tick re-show | "Who hides a ghost: found" |
| Part 4 (audit advice not to take) | not taken | as the plan says | |

## Things to know
- Every game address is in `src/game/engine.h`. A function the mod calls directly gets its first bytes checked in
  `src/game/fingerprint.cpp` (bytes from the dump); a mismatch turns that feature off or refuses the build.
- Poses are written on the client replay updater hook (`beforeClientReplayUpdate`), never the render thread.
- The game hides ghost client characters ~0.25 s after OnHideMap (`PamClientCharacterEntity::vf2`, property 0x35bc69ab);
  the setVisible hook notices and the next tick shows the ghost again.
- Parking (`kParkDepth`) hides hair that `setVisible(false)` leaves drawn; it stays until 3.6 is verified.
- Risk to watch in 2.9: `ReplayPlayback_Stop` releases the decoder and the keyframe ring on the server tick. Ghidra shows no
  message or unspawn inside it, but whether any other path (an end-of-playback callback) reacts to the state change was not read
  (research request R6).
- Ghidra names that are wrong or missing (not changed): 0x142105510 "JumpTuningRoot" is the registry's object vector;
  `FUN_143119500` registry add partition, `FUN_14311b8e0` remove partition, `FUN_143134420` add listener.

## Test checklist (what to run, what to send back)
Build the branch, play two players (host + one joiner, or the test server), keep `mecmp.log`, and send it back with the item
numbers. "never" = a log line that must not appear. Items overlap; one or two sessions cover them.

1. **Startup (all).** Log has "hook ghost setVisible log: installed" and "hook client spawner add: installed". Any
   "build check: ..." line (never, except on a changed game build) names the item that turned itself off.
2. **3.3 spawn hook.** Join, let the other player's ghost appear. Open the map and look at "Your Echo". Expect: the echo never
   turns into the player's ghost and the ghost keeps moving. Never "list reordered by the safety net" (if it shows, the hook
   missed; send the log). "list reordered as a character was added" is fine (verbose logging only).
3. **2.2 anchor pin.** Hide a ghost (player leaves, or F12) and show it again. Expect: the body drops out of sight and comes
   back; no ghost left standing where the player was; no flicker when it returns.
4. **2.9 server playback stop.** With a second player, a ghost that came from the game's city ghosts (not the mod's own spawner)
   follows that player as before. Never "build check: ReplayPlayback_Stop". If a ghost freezes, vanishes, or the game crashes soon
   after a ghost is first driven, send the log: that points at this change.
5. **2.4 / 2.5 bad pose.** Normal play; look for "rejects the pose" lines. A ghost that stands still while its player moves is
   the symptom to report.
6. **2.6 / 2.7 looks across levels.** Pick an Echo look or model for a ghost, change level (or enter and leave a time trial),
   pick again. Expect: the look applies each time, the game's own Echo/placeholder pieces look normal afterwards, no crash.
7. **2.8 spawn cooldown.** Join right after a level load, and again right after a ghost was removed. Expect: the ghost appears
   a moment later; the log shows a wait, not spawner failure counts that blacklist the spawner.
8. **2.1 long session.** 30 minutes with a ghost driven: memory (Task Manager) stays flat, no growth per level change; the log
   line "pose setup ... now has" appears once per ghost.
9. **Time trial.** Run a time trial with a player ghost near the game's own trial ghost: both show, neither takes the other's pose.
10. **Map close.** Open and close the map 10 times: ghosts must not vanish for more than a moment.
11. **3.4 registry check.** In the mod menu turn on "Look like this in my own game too" and pick an Echo look (or the Prologue
    outfit); that makes the mod search for the live look pieces. Expect "look: N look pieces are loaded" and then a "registry
    check:" line and a "class-id range" line. Send both from at least two levels (change level, pick a look again). "MATCH" means
    the registry could replace the heap scan; "DIFFERENT" names what differs; "not readable" means the layout differs from the
    dump. If looks stop working after this build, send the log.
12. **Look list from the registry.** After a level load, open the mod menu's look picker. The log says "model: N ghost looks
    found ..." with "from the data registry", "from a time-trial marker" or "by memory scan". Expect the first two. "By memory
    scan" on every level, or a shorter or different look list than before this build, is this change's bug: send the log.
13. **UI system.** Open and close the pause menu and the map once: the game pauses as before. Never "UI system global ...
    differs" (if never, the update hook can go).
14. **Own spawner (research, optional).** Set `own_spawner=1`, open the map, note whether the echo plays on a MECMP ghost.
    This records whether 1.1's failure is real before the code goes (plan 1.1).
15. **Hair hide (research, optional, needs a code change first).** 3.6 needs an experiment that sets the flag byte of every entry on
    a hidden ghost's customization component; not written yet (see "0.6 / 3.6" in progress.md).

## Where the evidence is
- `docs/progress.md`: results per item, in order; "Ghidra pass 2" has the latest reads (addresses, offsets, what was and was not
  shown).
- `docs/research-requests.md`: the long reads that remain, written so another reader can answer them.
- `docs/ARCHITECTURE.md`: the ghost pipeline notes (client-thread writes, setVisible hook, spawn trigger).

## What is left (2026-11-16)
- Tests not yet run: checklist 8 (30-minute memory), 5 (a client that rejects a pose), 7 (spawn cooldown), 14 (`own_spawner=1` result: known to
  fail on the map echo, see progress "1.1"), 4 with a game city ghost adopted by a second player.
- Code not done: 3.6 (hair via attachment flag), 1.4 (shared data patched in place), 1.1 (own spawner), 3.4 remainder (echo
  look list and entity scans from the registry), 2.6 look cache across levels, 2.10 look-apply per-ghost time.
- Research for 1.1, 1.4, 3.6, 3.4: `docs/research-requests.md` (R1-R10).
- Fixed after the first test (all confirmed in game): game echoes in a T-pose after a level change (patch record kept across
  levels), invisible after a checkpoint reload with your own look (piece scan waits for the player).

## Research in flight (started 2026-11-16)
The user gave `docs/research-requests.md` (R1-R10) to Gemini. Its answers are NOT in the repo yet. Next session:
1. Ask the user for Gemini's answers (they may paste them or add a file).
2. Treat every claim as a hypothesis (GridNode rule): check each in Ghidra (decompile the named function, read the bytes) before
   it goes into `src/game/engine.h`, a comment or a doc as fact. Record per question: verified / wrong / not shown, with the address.
3. Order of value: R3 + R10 (hair, unlocks 3.6), R8 (registry gaps, "level loaded" event), R9 (echo look list), then R1/R2/R4/R6/R7
   (1.1, 1.4, 0.4, 2.9 safety, map-close writer), R5 (3.4 entity lists).
4. What each answer would change in code: R3/R10 -> replace `kParkDepth` parking with an attachment flag (3.6); R8 -> read the piece
   list at level-loaded instead of waiting for the player; R9 -> echo looks from the registry at every level start; R1/R2 -> own
   spawner / copy the shared setup data (1.1, 1.4); R4 -> 0.4/1.3 bus-id claim; R6 -> keeps or drops 2.9; R7 -> removes the map-close
   workaround; R5 -> replaces the entity heap scans.
Status of the research answers: pending (none received).
