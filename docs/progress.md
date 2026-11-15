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
