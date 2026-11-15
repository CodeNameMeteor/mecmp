# Progress: GridNode implementation plan (branch gn/implementation-plan)

Plan: MECMP-implementation-plan-2026-11-15.md (against main 7195f45). PR #118 (first batch) is merged into main; the rest is in
the draft PR after it. Nothing was compiled in the container; see docs/handover.md for the status table of every item, why each
open item is open, and the test checklist. Open research questions: docs/research-requests.md.

The lists below are the working log, in the order the work was done. handover.md is the current summary.

## Done (log; 3.1 and 3.5 tested, the rest untested)
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

## Not done yet (updated 2026-11-15)
- 3.6: replace the 1000 m parking hide with an attachment flag. Checked (see "0.6 / 3.6"), not implemented: needs a test
  that the hair is one of the customization component's attachment entries.
- 3.3 follow-up: remove the OnHideMap double refresh only after a game test shows the safety-net log line never fires.
- 3.4 beyond the check below (replace the scans), 1.4 (shared data patched in place),
  1.1 (own spawner), 2.10 per-ghost look-apply time (`lastLookApply`; unclear what the plan wants, left alone).
- 2.6 third bullet: keep the look cache across levels (see "2.6").
- 3.1 as first written here is superseded by "3.1 correction" below: the trigger is ServerPlayerChangedCharacterMessage.

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
- 0.6 checked 2026-11-15, see "Ghidra pass 2" at the end of this file. 0.1, 0.2, 0.4, 0.7 also have updates there.

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

### 2.2 rest (done, untested)
`fixClient` is gone: the server tick calls `setPin` (stores the anchor position in the ghost under g_m) and
`beforeClientReplayUpdate` writes it with `pinAnchor` on the client update thread, under the same condition as before (not
once renderFrame drives the pose every frame). A parked ghost's anchor moves the same way. Test: hide a ghost (player leaves
or F12) and show it again; the body should drop below the map and come back, and no ghost should be left standing where the
player was.

### 0.6 / 3.6 (Ghidra, 2026-11-15): checked, not implemented
`fb::PamClientCharacterCustomizationComponent::vf64` (0x14391cf60) runs every update: after the base `vf64` it walks the
16-byte entries of the vector at component +0x340/+0x348 and writes `entry[0] + 0xca = (entry[8] == 0 && FUN_143912b50() == 0)`.
So an attachment's visibility byte is rewritten each update from a flag at entry +8: a nonzero flag would keep it hidden
without a per-frame write. The mesh builder (0x143947e10) only bakes component +0x334 / +0x335 into each render instance when
it is built (+0x100 and the 0x80 flag), so those are no runtime switch.
Not shown: that the hair is one of those entries, what else sets entry +8, or what FUN_143912b50 returns for a ghost. Parking
(kParkDepth / kParkMs) stays until a test confirms it: set entry +8 of every entry on a hidden ghost from the client updater
hook and look whether the hair goes. Plan 3.6 itself says parking works today.

### 2.6 (done, untested; gate 0.5 checked in Ghidra, 2026-11-15)
Gate 0.5: `onCharacterSpawned` (0x1432aa460) does add and release a reference, but on the spawn template's data container
(spawner +0x30 -> vtbl[1]), not on our CustomizeCharacterData, and the pair is balanced. The apply path
(`PamServerCharacterEntity_applyCustomization` 0x14336cf00 -> `applyCustomizeData` 0x14336c860) only reads +0x18, +0x20,
+0x30, +0x31 and never touches +0x08. So no extra reference is needed to keep the data alive.
`DataContainer_release` (0x142a91cd0): +0x08 is a u16 reference count and +0x0a u16 flags; at zero it runs the destructor and,
without flag 0x2000, poolFrees the object. `makeCustomization` now copies the preset from +0x18 (not its header) and sets
count 0x4000 and flags 0x2000 itself, so a stray release can neither reach zero nor free our calloc memory.
Not done: keeping the cache across levels keyed by asset name. The cached data points at the old level's VisualUnlockAssets,
which may be gone; reuse needs a check that the pieces still exist. The per-level leak stays (one small block set per look).

### 2.9 (done, untested)
`drive` no longer only writes `SRP_PlaybackState = 0`: `stopServerPlayback` calls `ReplayPlayback_Stop` (0x143970cc0, on
component + 0x30) when the state is non-zero, then writes 0. Ghidra: Stop does nothing when state (+8) is 0; otherwise it
releases the decoder (+0x30, vtable +0xb8, poolFree), zeroes +0x30/+0x10/+0x20/+0x08 and frees the keyframe ring (+0x48).
So a ghost adopted from the game with a loaded recording no longer keeps both. The client keeps the plain write (plan H8:
the client's stop tells the game the replay is done). The call is guarded by `fingerprint::checkReplayStop()` (first 12
bytes, from the dump); on a mismatch it logs and falls back to the old write. Unverified: that no game thread uses the ring
while the server tick frees it (the old write had the same kind of race, but never freed anything). Test: join with a second
player, look for a ghost that was adopted from the city ghosts (not our own spawner); it should follow the player as before,
and the log should show no "build check: ReplayPlayback_Stop" line.

### 3.3 (done, untested)
`ClientCharacterSpawnEntity` vtable +0xd8 (`onCharacterSpawned`, 0x1432aa460, Ghidra: applies the spawn template to the
character, then `ClientSpawnEntity_addSpawned`) is wrapped by `onCharacterSpawnedHook` (`hookSpawnerAdd`, installed next to
`hookSetVisibleLog`; only if the slot still holds that function, else it logs and does nothing). After the game's add it
reorders that one spawner's list: our ghost characters first, the game's own last (`gameCharactersLast`, `game/spawn_order.h`,
host test `tests/spawn_order_test.cpp`, passes). Our character is claimed when it is created, which is before this add, so
it is already known. The hook uses try_lock on g_m and skips on contention. The 250 ms pass stays as a safety net and its log
line now says "reordered by the safety net". The OnHideMap double refresh is NOT removed (the plan: only after a game test
shows the safety net never fires).
Unverified: that the claim really happens before the add in every spawn path (the log line shows it if not), and which
thread calls the add.

### 3.4 research and check (Ghidra 2026-11-15; check done, untested; scans NOT replaced yet)
The plan's "object table 0x142105510" is real but is a field, not the registry. The registry object is at 0x1421054b0
(`Registry_Object`); Ghidra's label "JumpTuningRoot" on 0x142105510 is wrong. From `FUN_143119500` (adds a loaded partition's
containers, called with the partition's container array at +0x18 and a u16 partition id) and `FUN_14311b8e0` (removes them):
- 0x142105510 / +8 / +0x10: begin / end / capacity of a sorted vector of 16-byte entries `{u64 key, DataContainer*}`;
  key = partition << 40 | classId << 16 | index; classId = u16 at +0x28 of the object's type (the object's vtable slot 0, as in
  `Registry::typeOf`). The runtime dump has begin 0x14e7a1110, end 0x14e7a9b10 = 2208 entries (GridNode counted 3,618 in a level).
- 0x1421054b0 / +8 / +0x10: a second sorted vector, 8-byte {u32 partition << 16 | classId, u16 running index} (the per-class counter).
- 0x1421054e0: red-black tree of all container pointers (count at 0x142105500). 0x142105540: red-black tree of listeners (node +0x20
  context, +0x28 function); `0x143134420(registry, &{context, fn})` inserts into it. Dispatch (both functions): `fn(context,
  vector<DataContainer*>*, u16 partition, bool added)`: 1 on load, 0 on unload (read from the call, not verified in game).
- The loader holds a lock around all of this (0x1423f7390, taken by FUN_142a9f970, released by FUN_142aa1840). The mod does not
  take it, so a read can be torn while a level loads: `registry::objects` checks the three pointers, and callers check each object.
- Entities (`FUN_143164d50`, ClassInfo_firstInWorld): the head of a class's list in a world is the pointer at
  `world + (classId + 6) * 0x10`. Not followed further (`nextSubclass` not read). Components are still not known to be in such lists (T5).
Done in code (no behaviour change): `models::reset()` now clears `g_seen` (the look-picker entities of the old level; it runs
before the new level's entities are created, like the other factory-hooked lists cleared in `level::reset`);
`game/data_registry.h/.cpp` reads the table (engine.h `Registry_*`); `findPieces` ends its heap scan with `compareWithRegistry`,
which logs "registry check: MATCH" when the registry holds exactly the live pieces the scan picked and none it missed (else
"DIFFERENT (send this log)" with up to five names each way, and the class id of `VisualUnlockAsset`). Host test:
`tests/data_registry_test.cpp` (key decoding, table plausibility; passes).
Next, only after MATCH logs from a few levels: replace the scan in `findPieces` with the registry (keep the scan as a fallback
that logs when used), then a listener (`Fn_Registry_AddListener`) for unloads; then `scanForCatalogue`; entities last.

## Ghidra pass 2 (2026-11-15, MirrorsEdgeCatalyst_dump_SCY.exe; no code changed)
Read in the decompiler only; nothing here was seen in game.

**0.1 (rest): confirmed.** `Arena_allocFromMember` (0x142a647d0): when `*arenaMember` is null it stores `FUN_142a66490()` there and
allocates from it. That function returns the arena for the page the pointer belongs to, else the global default arena object
(vtable object 0x1423606d0, the same one the bus lookups pass). So a null member means the shared default arena, and the pose
buffers `preparePose` allocates (2.1) come from it.

**0.2 (rest): confirmed.** The only consumer of CRP_CountOk (+0x2e4) and +0x2e5 is `PamClientReplayPlayerComponent_update`
(0x1439501a0): `ReplayPlayback_Tick` runs every update; the received pose (+0x180) is copied to +0x1d8 and the body placed
only when BOTH flags are set (+0x2e6 skipRoot picks the placing path). A pose with the wrong count is not applied; there is no
other effect. 2.4's `continue` is therefore exactly "this client ignores it".

**0.4 (rest): partly.** `PamClientCharacterEntity::vf65` (0x143916b40) looks up its anim entity with the character's data
(+0x30) as the key and a bus-like object as the last argument (Ghidra calls it `physicsHolder`; it is the +0x28 MECMP calls
`Character_Bus`); it also copies `+0x335` from the character data (+0xe9) and `ghostShader` (+0x334) from data +0xe8, so +0x334/+0x335
are per-entity flags set at init, as 0.6 assumed. `PamServerReplayEntity` keeps its data at +0x28 and its bus at +0x20 (one slot
earlier than the character). Still not shown: whether the character's +0x28 object is the spawn bus or a child bus.

**0.6: partly confirmed.**
- `vf64` of `PamClientCharacterCustomizationComponent` (0x14391cf60) and the base `ClientCharacterCustomizationComponent::vf64`
  (0x14322aad0) both write `attachment + 0xca = (entry.flag @ +8 == 0 && hideAll == 0)` for every 16-byte entry
  `{attachment*, flag}` of the vector at component +0x340/+0x348, every update. The base one also calls the attachment's
  vtable +200 (its update) first. So a nonzero flag keeps an attachment hidden for as long as the flag stays.
- `hideAll` is `FUN_143912b50` (0x143912b50): 1 when a global manager (FUN_143269ae0, vtable +200 -> FUN_14319d9b0) says so;
  else for a component with a player at +0x238 (+0x1398 clear, +0x1520 bit 2) a value from the player's state; else the
  result of vtable +0x78 of the object at component +0x1d8 (not read). For a ghost (no player) the last path decides: unknown.
- The entries are filled through the component's vtable +0x208 from the character's `VisualUnlockAsset` list (field +0x60),
  so a hair piece, which is a VisualUnlockAsset, very likely is an entry. Not proven; nothing was found that writes the flag
  at +8 (no direct write in the customization component code range), so what sets it is unknown.
- `vf4` (0x143911510) forwards six message ids (0x37848c36, -0x45ef1ee0, -0x21db91a7, 0x28161398, 0x71ade322, 0x77da2e7a) to
  every attachment (handlers FUN_14304e580, 1430503a0, 14304ed30, 14304e480, 14304ffd0, 1430502b0): enable/disable-style
  handlers that set a state word at +0xc6 and call the attachment's sub-object at +0x68 (vtable +0x88 / +0x60). Another way to
  hide an attachment, not tried.
- For 3.6: a test in game is still needed (set the flag byte of every entry on a hidden ghost from the client updater hook and
  see whether the hair goes). The Ghidra side supports it; it cannot prove the hair is an entry.

**0.7: inconclusive, but the destroyer is found.** The code that removes a replay ghost is `PamServerReplayEntity_stopReplay`
(0x1439741d0): it finds the spawner by link 0xa695a747 (data key in the bus) and calls the spawner's vtable +0x108 (unspawn)
on its LAST spawned character. Slot 3 (0x143971b80) was not shown to destroy anything.

**1.2: confirmed in both directions.** `PamServerReplayEntity_startReplay` (0x14396e8b0): finds the spawner the same way and
spawns (vtable +0xd0) only when `ServerSpawnEntity_getLastSpawnedCharacter` is null; otherwise it returns that character
(adopts it). `stopReplay` unspawns the last one. So while the city ghost spawner's list holds only MECMP ghosts, the game's next
StartReplay adopts one and the next StopReplay removes it. This is the plan's mechanism exactly.

**1.1: partly.** The spawner lookup (`FUN_14311c850` -> `FUN_143145050` -> `FUN_14314e090`) is a binary search (lower bound) in
a bus's sorted key array (bus +0x68 -> +0xd0, or +0xf8 in the other mode), parallel entity array at bus +0x40; it returns ONE entity
and walks to the parent bus when the key is absent. A second spawner with the same data key in the same bus therefore sits next
to the game's in that array; which of the two a lower-bound search returns depends on how the registration inserts equal
keys, which this read does not show. GridNode's "found instead of" is plausible, not shown. The plan's test (`own_spawner=1`
with the map open) still decides it. Also: the registry's "register" function is the partition loader `FUN_143119500` (adds a
whole partition's containers, 3.4 section above), so registering one custom data object means building a partition-like
container array; not read further.

**3.4 (rest).**
- The look catalogue's data class is `fb::PamEchoCustomizationDefinitionsMeta`, vtable 0x141c96678; it is a data container, so the
  registry should hold it. `scanForCatalogue` could then read the registry for that vtable instead of scanning for the picker
  entity's vtable.
- The class-id range of a type can be read from the image: `TI_VisualUnlockAsset.classId` is the u16 at 0x14281e9b8 and
  `lastSubClassId` at 0x14281e9ba. A registry entry belongs to the class when `classIdOf(key)` is in that range (sub-classes
  included), so findPieces would not need MECMP's own vtable guess (`g_pieceVtable`).
- Entities: the head of a class's list in a world is at `world + (classId + 6) * 0x10` (`FUN_143164d50`). `ClassInfo_nextSubclass`
  and whether replay components are in those lists (T5) were not read.
- Ghidra labels to fix by the user (not changed here): 0x142105510 "JumpTuningRoot" is the registry's object vector;
  `FUN_143119500` = registry add partition, `FUN_14311b8e0` = registry remove partition, `FUN_143134420` = registry add listener.

### 3.4 additions and 3.7 (2026-11-15, after Ghidra pass 2; untested)
- `registry check` now also logs the class-id range of `VisualUnlockAsset` read from the image (engine.h
  `TI_VisualUnlockAsset_ClassId` / `_LastSubClassId`): how many registry objects fall in it, how many of those have another vtable
  (sub-classes), and how many objects with the piece vtable are outside it. Log only.
- `findEchoes` now tries the registry before the heap scan: `catalogueFromRegistry` looks for a `PamEchoCustomizationDefinitionsMeta`
  (vtable 0x141c96678, engine.h `Vtbl_PamEchoCustomizationDefinitionsMeta`) and reads its catalogue with the same code as the picker
  entity path (`readCatalogueMeta`). It is read-only and throttled to once a second; an unreadable table or no meta found falls
  through to the scan as before. Log: "model: N ghost looks found from the data registry". UNVERIFIED: that the vtable constant is the
  object's first qword (the symbol address of the vftable, same convention as the other `Vtbl_` constants) and that the registry's
  meta holds the same list as the picker entity's. If the log says "by memory scan" every time, the constant or the registry path
  missed; if the look list differs from before, this is the cause.
- 2.10 look-apply: left as is. `lastLookApply` is the intended "one mesh rebuild per tick" throttle (its comment says so); the
  per-ghost retry already uses `g.lookTriedAt`.
- 3.7 was already done in the code: `gameMenuOpen` reads the global (0x1424157f0) and the update hook is only a fallback that logs
  once ("UI system global ... differs") if they ever disagree. The hook can be dropped once a game log shows no such line.
