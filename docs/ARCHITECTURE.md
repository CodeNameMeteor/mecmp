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
   build was made from, and run the build check (`game/fingerprint.cpp`): six vtable slots must
   hold the expected functions and five directly called functions must start with the expected
   bytes. Any mismatch: stay inactive (`[debug] ignore_build_check=1` overrides).
2. Read `mecmp.ini` (`core/config.cpp`).
3. Install hooks (`game/hooks.cpp`): the tick hook is checked first and is required; spawner hooks
   are optional (the memory scan covers them); without the two client hooks ghosts are not spawned.
   Every slot is checked before anything is written. If the tick hook cannot be installed, the
   factory hooks already in place are taken out again, so a failed start leaves the game as it was.
   Then start the network thread (`net/net.cpp`). The DLL pins itself so it is never unloaded.
4. The mod thread then watches for level changes and hotkeys, and scans game memory for spawners
   when they were missed (`game/level.cpp`, see `scanForMissing`): 15 s into a level and, while
   something is still missing, after about 1, 3 and 7 more minutes (at most 4 scans per level).
5. When the game window is destroyed, multiplayer stops and the router port is closed (2 s at most).

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
   **Placement**: the client update (`PamClientReplayPlayerComponent_update`, 0x1439501a0) puts the
   body at `bone0.t + bone0.t.w * rotate(bone0.q, bone1.t)`. Bone 0 is the identity, but decodes
   from the wire as a ~0.14° tilt; with the world position written into bone 1 as is, the body was off by that angle
   times the distance from the level's origin (metres, growing with height; a pause, which stops
   this update, put it right). Something else takes bone 1 alone as the position (seen while paused),
   so bone 0 is folded into bone 1 (bone 0 = identity, bone 1 = {pos, bone0.q * bone1.q}): both then
   agree, where two different answers made the body jitter (`pose_math.h`, `tests/pose_test.cpp`).
   **Per-frame poses** are written by the client replay updater itself: each of its two updater
   objects (`*0x142578588`, `*0x142578590`, vtable 0x141c49190) gets a vtable copy whose slot 2
   (`updateAll`, 0x14394da40) first calls `ghosts::beforeClientReplayUpdate()`. Written from Present
   (render thread), the game could read a pose half old and half new.
   Present still writes them until the updater objects exist.
4. **Visibility**: ghost client characters start hidden (time-trial markers normally show them).
   Call `clientCharacter->vtbl[0x3d8](true)` after creation and every few seconds; `false` when
   the player stops sending.
5. **Lifecycle**: the mod never destroys a ghost (no known safe despawn call). A player silent for
   5 s has their ghost hidden and no longer driven; the ghost is then free and goes to the next
   player who needs one, preferring a player with the same name (a reconnect gets a new session
   id). At most 15 ghosts per level. F9 shows the ghost tick's cost (average and maximum).

Claiming client objects: inside the 1.5 s window after a spawn, a client character must have the
ghost blueprint's character data, and a client replay component must belong to such a character
(Component +0x08 is the owner's component collection, whose first entry is the owner). The ghost
blueprint's character data is taken from `ObjectBlueprint::Object` and accepted only if it is a
`PamCharacterEntityData`; after the spawn the character's own data confirms it.

Known limit (C-2): client objects are used from the server thread. Each use first checks the
object's class, data and owner, so memory the game freed and reused for something else is not
called into, but the client thread can still destroy an object between that check and the call.
Closing that window needs the client work moved to the client thread.

## Looks (`game/models.cpp`)

A look is the set of VisualUnlockAssets in a character's `ServerCharacterCustomizationComponent`
plus a CustomizeCharacterData's unlocks. The mod builds its own `CustomizeCharacterData` (one group
per piece, the source's unlocks, ClearVisualState = 1) and applies it with `character->vtbl[0x340]`
(`0x14336cf00`, args `{character, player = null, customize, skip = 0}`); the look replicates.

- **Faith**: the local player's current outfit (first-person `_1p` pieces dropped).
- **Echo looks** (the game's runner-ghost styles x colours): the catalogue comes from a time-trial
  marker's `PamServerCustomizeProjectionCharacterEntity` (vtable `0x141c7ea80`), found by a short memory
  scan 20 s into a level: entity `+0x28` data, data `+0x20` `PamEchoCustomizationDefinitionsMeta`,
  meta `+0x18` array of `{NameSid, DescriptionSid, TextureId, CustomizationData*, u32 CustomizationHash}`.
  An echo look's colour is an unlock (an ObjectVariation on the projection shader), so the unlocks are
  carried over. The hash is the game's own id for the look.
- **Models**: any CustomizeCharacterData from a spawner template in the level (Faith variants, RunVis).
- `PamUseRunnersGhostShader` (character data +0xe8) is turned off while a normal look is built
  (with the ghost shader on, normal meshes are invisible) and back on 3 s later, because the game's own
  time-trial ghosts share that data. Echo looks keep it on.
- Built customizations are cached per (pieces, unlocks) and the cache and the echo list are emptied on
  level change (level data).

Each player picks their own look (menu *Your look*: Faith, or a ghost style and colour; `[player]
look=` in mecmp.ini: `faith` or `echo:<hash>`) and sends it with their name. Every ghost shows its
player's look; there is no override on the receiving side. A look the catalogue does not know yet shows
as the default red ghost and is applied once the list is found. Some echo customizations carry no body
piece of their own (only colour unlocks): with ClearVisualState those left the ghost without a mesh, so
they are built on the RunVis body pieces. New picks default to red RunVis (easy to see). At most one look is applied per tick.

## Network (`net/`)

UDP, one socket. Sessions have one host and any number of joiners (up to 16).

- **Host** (`join=` empty): `upnp.cpp` finds the router (SSDP), asks it to forward the UDP port
  (SOAP AddPortMapping, 1 h lease, renewed every 30 min; never a permanent mapping) and reads its
  public IP. The SSDP answer's LOCATION and the control URL must point at the router that answered.
  The mapping is removed when the session switches to joining and when the game closes. Without UPnP a STUN request
  (stun.l.google.com) through the game socket finds the public address. `joincode.cpp` turns
  ip:port into a 10-character Crockford base32 code. The host forwards every joiner's packets
  unchanged to all other joiners, so only the host has to be reachable.
- **Join**: send to the host only; everyone else's packets arrive through the host.
- Packets: STATE ~30 Hz and HELLO (name and look: u32, 0 = Faith, else the echo hash) ~1 Hz. Remote players are keyed by the random session id
  in each packet. Pose compression: rotations smallest-three 32-bit, translations i16 at 1/1024 m,
  only for bones that changed since the last full packet (one per second); about 150 bytes.
- Receivers buffer 16 snapshots per player and show them 120 ms in the past, blended. The sender's
  32-bit millisecond clock is unwrapped (`protocol::ClockUnwrap`), so a 49-day uptime wrap is harmless. Positions and angles outside a sane range are dropped.
- Packet loss per player (`protocol::LossWindow`): gaps in their sequence numbers over the last 5 s;
  shown in the menu's player list. Join/leave notices (`ui/notices.cpp`) come from the same player
  list: a named player appearing, or silent for 5 s (as for their ghost).
- At most 64 player entries are kept; an entry silent for 2 minutes is forgotten. Packets of unknown
  type are dropped before anything is stored or forwarded.

## Name tags (`ui/nametags.cpp`, `game/camera.cpp`)

Each frame (render thread) the camera is read through `g_clientCameraViewManagers` (`0x142415e50`,
loaded by `clientWorldFrameTick`; the load is in the build check): `+0x08` vector of
`ClientCameraViewManager*` (one per local player), manager `+0x90` = final camera transform
(right, up, forward, position; the camera looks along minus forward), manager `+0x80` = the vertical
field of view the renderer uses, in degrees, with the player's FOV setting applied (the active camera's
`+0xac` is only its base value, 55). Each shown ghost's head (2.05 m above its trajectory bone) is projected with that
camera (`game/camera_math.h`, unit-tested on Linux) and its player's name drawn with ImGui: full
until 40 m, fading out by 150 m, smaller with distance. Names are drawn through walls.

## Settings (`core/config.cpp`)

`config::get()` returns a shared, immutable snapshot (a pointer copy, no string copies), so it is
cheap to call every tick. Changes (menu) are queued with
`config::change()` and applied in order by the mod thread every 50 ms, which also writes
`mecmp.ini`. No thread reads a setting while another writes it, and no file is written on the
render or game thread.

## Menu input (`ui/overlay.cpp`, `ui/gamepad.cpp`)

- Keyboard: the menu key (INSERT by default, changeable in the menu) toggles it in the window
  procedure. While open, mouse and keyboard go to the menu (virtual cursor from raw input).
- Gamepad: the game loads XInput itself and keeps `XInputGetState` in a data pointer
  (`0x14256d7e0`, written once by `0x1437ba6c0`). The mod swaps that pointer (the build check
  verifies the store instruction). Its function calls the real one, opens/closes the menu on the
  bound button combination (LS+RS by default), and while the menu is open hands the game a pad
  at rest (with a fresh packet number, so the game drops the values it held). After the
  combination closes the menu, its buttons stay hidden from the game until released. The
  controller-detection pointer (`0x142607db8`) is left alone. ImGui navigates the menu through its
  own XInput access.
- The join code only reaches the clipboard through the menu's *Copy* button.
- Developer hotkeys F7/F9/F12 exist only with the CMake option `MECMP_DEBUG_KEYS`.

## Logging

`mecmp.log` stops at 16 MB. Messages caused by other players (joins, leaves, new session ids,
names) share a budget of 20, then one every 3 s; each player's name is logged at most 3 times.
The exception logger uses no heap and never waits for the log lock.
