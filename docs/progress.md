# Progress: GridNode implementation plan (branch gn/implementation-plan)

Plan: MECMP-implementation-plan-2026-11-15.md (against main 7195f45). Nothing here was compiled or game-tested.

## Done (untested)
- Step 1: 1.5 (client spawner vtable comment: add +0xd8, remove +0xe0), 1.1 comment on `g_ownSpawner`,
  2.5 (skip ghost when pose read fails), 2.10 (`g_placed` duplicate push).
- Step 2: 2.1 (`preparePose` allocates once at kMaxBones), 2.3 (render-thread fallback removed), 2.7
  (`restoreDonors()` before `g_own = Own{}`).

## Not done yet
- 2.2 (move `fixClient` into `beforeClientReplayUpdate`): needs care with the ghost list lock; do with a build.
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
