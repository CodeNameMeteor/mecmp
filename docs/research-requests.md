# Open research questions (for a second reader, e.g. Gemini)

Target: the Mirror's Edge Catalyst PC image (EA app build), image base 0x140000000, addresses are image VAs.
Rules for the answer: separate **verified** facts (quote the decompiled lines or bytes, with the function address) from
**hypotheses**. A hypothesis is not an annotation until someone confirms it in Ghidra. Names in the dump are partly wrong
(see the last section). Do not guess offsets; say "not shown" when a field's writer is not in what you read.

Background: MECMP shows remote players as ghosts by driving the game's replay ("echo") client characters. The mod reads
poses from the network and writes them into the client replay component each update. See `docs/ARCHITECTURE.md` and
`docs/progress.md` ("Ghidra pass 2") for what is already established.

## R1 (plan 1.1): own spawner, data registration
Why: `own_spawner=1` makes MECMP register its own spawner; it fails today. A working one needs data the game finds in its
registry. Questions:
1. `FUN_143119500` (add a partition to the data registry) and `FUN_14311b8e0` (remove): what does a partition hold, who calls
   them, and could a mod call the add with its own partition holding one DataContainer of the spawner's class?
2. The spawner lookup is a binary search in a bus's sorted key array. When two entries have an equal key, which one does the
   search return (first, last, any)? Show the compare and the loop.
3. The registry insertion (sorted vector at 0x142105510, 16-byte `{key, DataContainer*}`, key = partition<<40 | classId<<16 |
   index): where is an entry inserted and how is the order among equal keys decided?

## R2 (plan 1.4): shared data patched in place
Why: the mod patches shared replay setup data in place; the plan wants the data copied first.
1. `ReplaySetup_Build`: list every argument and which client component or data field each comes from. In particular where the
   rig table comes from and its element size.
2. How are the pose buffers sized (what decides the count and the arena)? Which fields hold the sizes?
3. Is the setup data shared between all ghosts of a level, or created per character? Show the creation and the free.

## R3 (plan 3.6 / 0.6): hair and the customization entries
Why: hidden ghosts keep hair drawn; the mod parks the body deep underground to hide it. 3.6 would use a flag instead.
1. Customization component vf64 (rewrites attachment +0xca each update from entry flag +8; entries are 16 bytes in a vector at
   +0x340/+0x348): which function writes the entry's byte at +8, and under what condition?
2. `FUN_143912b50`: what does the object at component+0x1d8 (vtable +0x78) return for a ghost (client replay character)?
3. Is hair an entry in that vector, or a separate attachment? Which attachment events (0x37848c36 and neighbours) are raised
   and who handles them?

## R4 (plan 0.4): what is character +0x28
1. In a client replay character, is +0x28 the spawn bus itself or a child bus? Show where it is written (constructor or
   spawn) and the class of the object it points to.
2. How is the bus id (the "unique id" the spawner claim uses) derived from it?

## R5 (plan 3.4): lists of live objects
1. `ClassInfo_nextSubclass`: what does it return and how are subclass lists terminated? Do replay components (client replay
   component, customization component) appear in the world's class lists (entity class head at world+(classId+6)*0x10)?
2. Registry listener `FUN_143134420(ctx, vector*, u16 partition, bool added)`: when is the callback called (loader thread? under
   the lock at 0x1423f7390?) and with what vector?
3. Is the registry object's first qword equal to 0x141c96678? (Likely not: that address is `PamEchoCustomizationDefinitionsMeta`.)

## R6 (plan 2.9): is it safe to stop server playback on the server tick
`ReplayPlayback_Stop` (0x143970cc0) releases the decoder and the keyframe ring.
1. Does any other thread or callback read the ring or decoder after Stop (an end-of-playback handler, the client side)?
2. Does a state change in Stop raise a message or unspawn anything? (Ghidra pass 2 saw none inside the function itself.)

## R7: map close
Who writes property 0x35bc69ab = false on the ghost client characters shortly after OnHideMap
(`PamClientCharacterEntity::vf2`)? The mod works around it; knowing the writer may allow a clean fix.

## Wrong or missing names in the dump
0x142105510 is labelled "JumpTuningRoot" but is the registry object's vector (object at 0x1421054b0). `FUN_143119500` =
registry add partition, `FUN_14311b8e0` = remove partition, `FUN_143134420` = add listener.

## Status 2026-11-18
Gemini answered R1-R10 (checked in Ghidra, see progress.md "Gemini answers R1-R10 checked"). Still open: the bus-id field and
whether character +0x28 is the spawn bus (R4.2); equal-key insertion order for a second spawner (R1.2, the answer cited the wrong
function; the insert code is not found yet); which thread runs MECMP's server tick relative to `FUN_143971ac0` (R6.1; after Stop a
Tick does nothing). R7 (who writes 0x35bc69ab) is closed as "data-driven, not findable from code".

## Answer format
For each question: `R#.n: verified | hypothesis | not shown`, the function address, the lines that show it, and what you
would check next in Ghidra.
