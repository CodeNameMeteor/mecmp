# How MECMP works

Target: the PC (EA app) build of Mirror's Edge Catalyst. All addresses live in
`src/game/engine.h` (image VAs at base `0x140000000`). The game runs its server and client
worlds in one process; the mod uses both.

## Rules the code follows

- No code patches. Hooks only swap pointers in vtables / entity-creator tables.
- Engine calls happen on the server game thread (inside the tick hook) unless noted.
- Fields the client thread also uses are written with small fault-tolerant writes (`game::poke`).

## Startup (`app/mod.cpp`)

1. Wait for the engine's TypeInfo list, then compare three TypeInfo addresses with the ones this
   build was made from. Any mismatch: stay inactive.
2. Read `mecmp.ini` (`core/config.cpp`).
3. Install hooks (`game/hooks.cpp`), start the network thread (`net/net.cpp`).
4. The mod thread then only watches for level changes and hotkeys.

## Hooks (`game/hooks.cpp`)

| Hook | Used for |
|---|---|
| Character spawner factories | capture the player's spawner (`Characters/FaithCharacter`) and the city-ghost spawner (`Characters/Projection/ProjectionCharacter`); collect spawn-template looks for models |
| Client replay player factory | claim our ghosts' client components; others give the reference bone setup |
| Client character factory | claim our ghosts' client characters (to make them visible) |
| `ServerGameWorld::postUpdate` | the per-tick update |

## Local player (`game/local_player.cpp`)

Position and facing come from the player's server character. The body pose comes from the
player's own replay recorder (`PamClientReplayRecordingComponent` +0x48), which always keeps
the last ~15 s for the game's "create a time trial" feature. Its newest keyframe is 23 bones of
`{vec4 translation, quat rotation}`, exactly what a ghost's replay player plays.

## Ghosts (`game/ghosts.cpp`)

Per remote player:

1. **Spawn** a ProjectionCharacter from the level's city-ghost spawner (`spawner->vtbl[0xd0]`),
   at the player's position. Give its server replay component a pose buffer with the bone setup
   real ghosts use (copied from any live ghost client component).
2. **Drive** (every tick): stop the server replay playback, copy the player's pose into the
   component, put the trajectory bone (setup index 1, the world anchor) at the player's position,
   mark the component dirty (bits 2|4). The engine replicates it to the client like a real replay.
3. **Client fixes** (every tick): a ghost client normally plays its own replay and parks the body.
   Stop its playback (+0x40, +0xb0), set skipRoot (+0x2e6) to 0 so the body is placed from the
   trajectory bone, and pin that bone in the received pose (+0x180) to the player's position.
4. **Visibility**: ghost client characters start hidden (time-trial markers normally show them).
   Call `clientCharacter->vtbl[0x3d8](true)` after creation and every few seconds; `false` when
   the player stops sending.

## Models (`game/models.cpp`)

A look is the set of VisualUnlockAssets in a character's `ServerCharacterCustomizationComponent`.
The mod builds its own `CustomizeCharacterData` (one group per piece, ClearVisualState = 1) and
applies it with `character->vtbl[0x340]`; the look replicates to the client.

- `faith`: the local player's current outfit (first-person `_1p` pieces dropped).
- Others: any CustomizeCharacterData from a spawner template in the level (RunVis, enemies, ...).
- `PamUseRunnersGhostShader` (character data +0xe8) is turned off for normal models; with the ghost
  shader on, normal meshes are invisible. It is turned back on for projection models.

## Network (`net/`)

UDP, one socket. Sessions have one host and any number of joiners (up to 16).

- **Host** (`join=` empty): `upnp.cpp` finds the router (SSDP), asks it to forward the UDP port
  (SOAP AddPortMapping, 1 h lease, renewed) and reads its public IP. Without UPnP a STUN request
  (stun.l.google.com) through the game socket finds the public address. `joincode.cpp` turns
  ip:port into a 10-character Crockford base32 code. The host forwards every joiner's packets
  unchanged to all other joiners, so only the host has to be reachable.
- **Join**: send to the host only; everyone else's packets arrive through the host.
- Packets: STATE ~30 Hz and HELLO (name) ~1 Hz. Remote players are keyed by the random session id
  in each packet. Pose compression: rotations smallest-three 32-bit, translations i16 at 1 mm,
  only for bones that changed since the last full packet (one per second); about 150 bytes.
- Receivers buffer 16 snapshots per player and show them 100 ms in the past, blended.
