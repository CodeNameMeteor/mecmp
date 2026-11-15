# Progress: GridNode implementation plan (branch gn/implementation-plan)

Plan: MECMP-implementation-plan-2026-11-15.md (against main 7195f45). Nothing here was compiled or game-tested.

## Done (untested)
- Step 1: 1.5 (client spawner vtable comment: add +0xd8, remove +0xe0), 1.1 comment on `g_ownSpawner`,
  2.5 (skip ghost when pose read fails), 2.10 (`g_placed` duplicate push).
- Step 2: 2.1 (`preparePose` allocates once at kMaxBones), 2.3 (render-thread fallback removed), 2.7
  (`restoreDonors()` before `g_own = Own{}`).

- 2.8 (spawn cooldown wait, gate 0.3 confirmed): `spawnGhost` returns before touching the spawner when +0xf0 > 0.01 and
  +0x108 bit 8 is set; no failure counted.
- 2.2 partly: playback state, forced flag and skipRoot pokes moved from `fixClient` to `settleClientPlayback`, called from
  `beforeClientReplayUpdate` for every ghost with a client component.

- 3.5: `level::playerCharacter()` tries the player manager first (`*0x14253d670 + 0x68` -> vector at +0x268/+0x270 ->
  ServerPlayer +0x1478 character, +0x1398 isHuman, character +0x250 must point back; Server_ctor 0x14333a650 stores the
  manager at +0x68, read in Ghidra), then the spawner path as before.
- 3.1: the server listener also subscribes to category Network (0xc67d277b) and counts NetworkOnPlayerSpawnedMessage
  (0xcab70d78; category and type ids are djb2-xor hashes of the names, which reproduce MECMP's known ids). Spawning now
  needs that message since the level started, or 150 ticks (5 s). The 1.5 s `kPlayerSettleTicks` wait stays until 1.2 /
  L4 explain the early deletion. UNVERIFIED: that this message goes through the SERVER manager and fires on every
  level load (GridNode only saw it at first boot). The log line "the player spawned message arrived" shows it.

## Not done yet
- 2.2 rest: the anchor/pose pin in `fixClient` (non-posing and parked ghosts) still runs on the server tick; it needs the
  net state and the park position on the client thread.
- 2.10 per-ghost look-apply time (`ghosts.cpp` `lastLookApply`): the static also caps mesh rebuilds to one per
  tick; per-ghost `lookTriedAt` already exists. Unclear what the plan wants; left alone.
- Steps 3-7 as in the plan; step 6/7 items wait on the Part 0 Ghidra checks.

## Part 0 results (Ghidra, MirrorsEdgeCatalyst_dump_SCY.exe, 2026-11-15)
- 0.1 confirmed. Server replay component dtor (0x14396bb10, `this` = component - 0x30) frees only the CURRENT pointers:
  pose (SRP_Pose) sized `(PoseCap - Pose) & ~0x1f`, flags sized `S+0x88 - S+0x78` (via 0x143945930), setup index
  array through `poolFree(S+0x08, S+0x10)` (no size). preparePose now keeps those size fields consistent.
  Still unchecked: whether `Arena_allocFromMember` with a null member uses a shared allocator.
- 0.2 confirmed in substance. readNetState 0x14394bde0 (`param_1` = client component + 0x1a8): CRP_CountOk (+0x2e4) =
  (8-bit count read from the stream == +0x200 setup count); +0x2e5 = 1 after a pose block. It writes the received
  poses WITHOUT checking the count (count*0x20 bytes into the received-pose buffer): a bigger server count than the
  client's buffer is an overflow, so never send more bones than the client setup has. Where +0x2e4 is consumed
  (what drops the pose) was not read.
- 0.3 confirmed. createCharacter 0x143390a30: refuses on `*(float*)(spawner+0xf0) > 0.01 && (spawner+0x108) & 8`;
  `belowMaxCount` is only called when `info->busId == -1`. With busId == -1 it also uses `(spawner+0xf4) & 0x7f`
  as the bus id and increments +0xf4 per spawn.
- 0.4 formula confirmed, applies to a different object than MECMP reads. fb_EntityBus_create -> 0x1431411d0 sets
  bus+0x58 low 25 bits = `(id + 0x1ffff80) & 0x1ffffff` for the bus createCharacter makes from `info->busId`.
  But that is the spawn bus handed to Blueprint_instantiate; MECMP reads the character's own bus at character+0x28,
  which may be a child bus with another id. That would explain GridNode's "same value for every ghost". Settle in
  game: log `character+0x28 == createdBus`? and +0x58 for two ghosts spawned with different busIds. 1.3 stays blocked.
- 0.5 not supported at this level. onCharacterSpawned 0x1432aa460 increments the refcount of the property's data
  container (+8) and releases it before returning: balanced. The claimed drop via 0x142a91cd0 is not in this function
  (may be inside Message_dtor / the sendMessage path). Do not pin the refcount yet.
- 0.7 inconclusive. Slot 3 (0x143971b80) sends message 0xc007a2e5 then calls 0x143167700, which only calls
  `(**(*owner)+0x10)(owner, obj)` unless a flag (bit 10 at +0x18) is set. Not clearly a character destroy.
- 0.8 readable. ReplaySetup_Build(arena, a2, a3, a4, a5, setup): a2 = ptr to a bone-id array (Frostbite array, count at
  [-1]); a3 = same shape, the flagged bones (flag byte = 1 at setup+0x78+slot); a4 = rig table (count at +8, remap
  table at +0xf0); a5 = root bone id or 0xffffffff; setup = out struct (+0x00/+0x18 count, +0x08/+0x20 arena,
  +0x10 index array, +0x28 inverse map, +0x38 parent array, +0x78 flags). Needs a4 (the rig): 1.4 option 1 is feasible
  only if MECMP can get that object from the client component's data.
- 0.6 not checked.

## Updaters (GridNode scratch/block-a-2026-11-15.md, spot-checked in Ghidra) - answers question A2
Checked: `updateAll` 0x14394da40 walks only ITS OWN component list (updater +0x28/+0x30) and calls
`PamClientReplayPlayerComponent_update` on each; `FUN_14394f600` (from StartEchoReplay with skipRoot=1) moves a component
from the pass-6 updater (`DAT_142578588`) to the pass-5 one (`DAT_142578590`) and sets comp +0x300 = 1; `vf37`
(component init) registers it by +0x300 and builds the setup from the data's +0x70 / +0x78 lists.
Taken from the report, not re-checked: both updaters share vtable 0x141c49190 (so MECMP's one patched slot is called
twice a frame, once per pass); `UpdateInfo` +0x10 is the pass number; pass 5 runs before `Client_updateFrame`, pass 6 after
pass 3; the recording updater is pass 1 (before pass 6).
- Part 4.1 (audit "once per frame"): each component is written to by exactly one updater, so the second hook call per frame
  is redundant for a pass-6 component. A ghost the game moved to pass 5 (echo) has +0x300 == 1: writing only before pass 6
  would put its pose one frame late, a plausible cause of MECMP's "less smooth" note (`ghosts.cpp` renderFrame). Not changed.
  The claimed-clientComp log lines now print +0x300 so a game run shows whether ghosts ever sit in pass 5. If they never do,
  writing once (pass 6) is safe; if they do, filter by +0x300 against `info->pass` in the hook.
- 0.8 / 1.4 detail from `vf37`: ReplaySetup_Build(arena = FUN_143164c80(comp), &data+0x70, &data+0x78, rig, 0xffffffff,
  setup = comp's expectedBones). The rig table is kept at comp +0x1d0. After it the component also sizes a pose buffer
  (+0x1d8, count*0x20) and the received-pose vector (+0x180/+0x188). So 1.4 option 1 is not a single call: those two must
  be redone too. Option 2 (patch around the game's create call) is confirmed correct in timing: the data is read at init.

## Players and characters (GridNode scratch/block-l-2026-11-15.md, spot-checked in Ghidra)
Checked: `ServerPlayer +0x1398` is set by `FUN_143358f00` and tested by nearly every case of the server player message
handler `FUN_143380660` (non-zero: return early). `addPlayer` `FUN_143349ce0` picks the human vector (+0x268) or the AI
vector (+0x4a0) by `+0x1399`, not +0x1398. So +0x1398 is "made for a character" (a wrapper), not "is human"; a real
player has it clear. `ServerPlayer_IsHuman` is renamed `ServerPlayer_MadeForCharacter`, and `characterFromPlayerManager()`
now prefers entries with it clear (it preferred set ones before: wrong). Still untested in game.
Not re-checked: the sender of NetworkOnPlayerSpawnedMessage (`FUN_143380660` is the server player message handler, not the
sender), and the L4 answer: early ghost deletion is `PamServerReplayEntity` StopReplay (0x1439741d0) unspawning the
spawner's LAST character, not the spawner's own update. If true, plan 1.2 (do not spawn from the city ghost spawner while
only MECMP ghosts are in its list) applies; not done yet.

### 3.1 correction (verified in Ghidra, from GridNode's report)
NetworkOnPlayerSpawnedMessage (0xcab70d78) is built on the stack in FUN_143380660's ServerPlayerChangedCharacterMessage
(0x40fa3f03) branch (ctor FUN_144663ac0, only caller) and sent to the peers by FUN_1433bfc20 on `*(ctx+0x70)`. It never
passes through the local message manager, so the Network-category listener could not fire (confirmed by the 15:52-15:59
log: no arrival line in 3 level loads). MECMP now listens for the trigger itself (category 0xf3c93dd3, type 0x40fa3f03; new
character set, old clear, 'same' flag clear, player is not AI and not made-for-character). Untested in game.

### Settle delay removed; map refresh kept (2026-11-15, user's logs e2b10af6, 45b9c8b5)
The 1.5 s wait after the player's character appears is gone: spawns 40 ms after the trigger survived (two level loads).
The show-again/re-look on OnHideMap was removed and put back: without it the ghost vanished after the map closed (no lost
or respawn line, so the character still existed, only hidden). User's guess: the game hides all ghosts when the map closes
(to remove the map's echo). Not checked in Ghidra yet: what OnHideMap / the Your Echo screen does to client ghosts.
OnHideMap (0x5b61f030) in Ghidra: only two code references to the constant. The message ctor FUN_144737390 and one
listener, FUN_143a49660 (OnShowMap -> FUN_143a4af60, OnHideMap -> FUN_143a40a50 + FUN_143a8c6f0), which belongs to
PamClientPlayerTagComponent (player tags / presence info), not to ghosts. So no game code hides client ghosts in direct
reaction to OnHideMap; whatever hides them runs from another path (map/Your Echo screen state). Keep the refresh.

### Diagnostic: who hides a ghost (2026-11-15)
Recordings `map` and `mp-map` (GridNode) hold no event aimed at a ghost character when the map closes; the user sees the
ghost vanish briefly (our show on OnHideMap and +1 s, and the 3 s re-show, bring it back). So `ghosts.cpp` now swaps the
`PamClientCharacterEntity::setVisible` slot (0x3d8) for a wrapper that logs the game's own hide calls on our ghost characters
(not ours): "ghost: the game hid ghost character ... (caller 0x14..., thread ...)", at most 40 lines. Behaviour unchanged.
Untested. Next: open Ghidra at the caller VA to see what code hides it.

### Who hides a ghost: found (2026-11-15, log a3c329bf)
The setVisible log shows the game hiding both ghost client characters ~0.25 s after OnHideMap (some closes, not all), always
from caller 0x1439199c1 = `fb::PamClientCharacterEntity::vf2` (vtable slot 2, 0x143919960), on one thread. vf2 is a property-change
handler: for property 0x35bc69ab it reads that property's bool (via the entity bus) and calls vtbl->setVisible(value); for
0x7c46ea68 it does something else (vf_0xe0 / vf105). So the game sets a bool property false on the character when the map
closes. Not seen in the GridNode recordings (no events on a ghost). Fix tried: the setVisible hook notes the game's hide
(`g_gameHidGhost`) and the next tick calls setVisible(true) on our ghosts again, so the gap shrinks to about a tick instead
of waiting for the 1 s refresh. The refresh on OnHideMap stays. Untested. Unknown: who writes the property (and why it is
false); the property's name (hash 0x35bc69ab) is not resolved.
Result (log 26d94fcc, user): with the next-tick re-show the user saw no flicker after map closes (hides logged 0.24 s after
4 of 7 closes, 3 characters each: two of ours and one more ghost-data character). Seen in the same log, not fixed: two
ghosts (two remote players) both logged "bus id 992a50" and both claimed their client characters by that bus id; they were
claimed 0 ms after their own spawn so they did not mix up, but it is the ambiguity plan 1.3 is about. Open question: is the
look re-apply on map close still needed now that visibility is handled? (Try removing it, keep the re-show.)

### 2.4 (done, untested)
renderFrame no longer writes a pose when the client's count flag (+0x2e4) is 0 (the client drops it anyway); drive already
sees the same flag on the next server tick and replicates again. The plan's "never fall back to referenceSetup once a ghost
has its own setup" was already true (drive keeps the ghost's current setup first). +0x2e5 (hasPose) not read: no verified
offset in engine.h.

### 0.4 / 1.3 / 3.2 (Ghidra, 2026-11-15)
createCharacter (0x143390a30) passes busId (or the spawner counter & 0x7f when -1) to fb_EntityBus_create, and
entityObject_initWithParentTransform stores `(id + 0x1ffff80) & 0x1ffffff` at that bus's +0x58: the audit's formula is real for
the bus created there. MECMP reads character +0x28 -> bus +0x58 and sees one value per spawner (logs 2026-11-15: two ghosts of
the city-ghost spawner, both 992a50; earlier single ghosts cac3a0, 16c7d60, ...), so what it reads is not that bus or
not that field: GridNode's reading holds for what MECMP uses. The claim code already copes: a bus-id match is used only
when exactly one ghost is waiting for its client character; two waiting ghosts fall back to the claim window. No code change.
Unchecked: which object character +0x28 points to.
