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
   when they were missed (`game/level.cpp`, see `scanForMissing`). A scan reads the whole heap (about
   a second of one core), so it is a fallback: in the level that was already running when the mod
   loaded, 15 s in and, while something is still missing, after about 1, 3 and 7 more minutes; in later
   levels (which the hooks see being built) at most once, 60 s in, and only in a level you play in.
5. When the game window is destroyed, multiplayer stops and the router port is closed (2 s at most).

## Hooks (`game/hooks.cpp`)

| Hook | Used for |
|---|---|
| Character spawner factories | capture the player's spawner (`Characters/FaithCharacter`) and the city-ghost spawner (`Characters/Projection/ProjectionCharacter`); collect spawn-template looks for models |
| Client replay player factory | claim our ghosts' client components; others give the reference bone setup |
| Client character factory | claim our ghosts' client characters (to make them visible) |
| Look-picker factory (`PamServerCustomizeProjectionCharacterEntity`) | the echo look catalogue, without a memory scan (optional) |
| `ServerGameWorld::postUpdate` | the per-tick update |
| `PamUISystem` update (vtable slot `0x141cb64f0`, `0x143b16c50`) | in a session, menus do not pause the game (below) |

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
   Not before the player's own character has existed for 3 s: a ghost spawned right after it appears
   is deleted by the game ~0.6 s later on every level load (the spawner drops it; cause not found).
   A ghost the game deletes is logged with what was found (`ghost: ... is gone`) and spawned again.
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
  marker's `PamServerCustomizeProjectionCharacterEntity` (vtable `0x141c7ea80`), noted as it is created
  (creator slot `0x141c7eb40`, create thunk `0x143a83490`) and read on the mod thread within a second;
  a memory scan is the fallback (20 s and 60 s into the first level, as above): entity `+0x28` data, data `+0x20` `PamEchoCustomizationDefinitionsMeta`,
  meta `+0x18` array of `{NameSid, DescriptionSid, TextureId, CustomizationData*, u32 CustomizationHash}`.
  An echo look's colour is an unlock (an ObjectVariation on the projection shader), so the unlocks are
  carried over. The hash is the game's own id for the look.
- **Models**: any CustomizeCharacterData from a spawner template in the level (Faith variants, RunVis).
- An echo look is applied from a full copy of its `PamCustomizeCharacterData` preset (vtable
  `0x141c883e0`, 0x70 bytes), not just its pieces, so the projection mesh and its colour variation come
  along.
- The ghost shader is set per ghost on its client character (`PamClientCharacterEntity` +0x334): on for
  echo looks, off for Faith and models (with it on, normal meshes are invisible). The shared character
  data flag (+0xe8, `PamUseRunnersGhostShader`) is no longer touched, so the game's own time-trial
  ghosts are unaffected. The client character copies the definition's flag while it is set up, which can be after it
  was claimed, so the tick puts it back to what the look needs (otherwise a Faith ghost showed only
  its hair).
- Built customizations are cached per (pieces, unlocks) and the cache and the echo list are emptied on
  level change (level data).

Every style other than RunVis is a `MeshAndVariationPair`: a separate projection body mesh
(`female_runner_projection_01_fullbody`) plus an ObjectVariation that swaps in
`FX_SS_Projection_<Style>` with MainColor/SecondaryColor. Built from pieces alone these showed nothing
in free roam; with the full preset copy and the per-ghost shader the menu offers every catalogued style
again (`models::echoRenders`: the look is in the catalogue).

**Known limitation (tested 2026-10-13):** the per-ghost flag is the *runner's* ghost shader, which draws
with the local player's own "Your Echo" style and colour. So every echo ghost shows in the viewer's own
echo look, and changing "Your Echo" changes everyone's ghost; the look a player picks only decides that
their ghost is an echo. With the flag off only RunVis renders (the other styles' assets are presumably
not loaded in free roam; time trials load a rival's style before showing it). Showing each player's own
style would need those assets loaded: open research. Also open: with the mod loaded, the game's own
"Your Echo" preview can appear and vanish at once; the ghost spawner's list does not change when it does,
so it is not the preview and our ghosts competing for that spawner. The cause is not known.

Each player picks their own look (menu *Your look*: Faith, or a ghost style and colour; `[player]
look=` in mecmp.ini: `faith` or `echo:<hash>`) and sends it with their name. Every ghost shows its
player's look; there is no override on the receiving side. A newly chosen look is applied on the next tick.
One the catalogue does not know yet leaves the ghost as it is (a new ghost starts as the default red
RunVis) and is retried every 15 s, not every tick: each attempt rebuilds the ghost's mesh. Faith and
other looks are retried every 2 s. When the catalogue is found, waiting looks are tried at once. Some echo customizations carry no body
piece of their own (only colour unlocks): with ClearVisualState those left the ghost without a mesh, so
they are built on the RunVis body pieces. New picks default to red RunVis (easy to see). At most one look is applied per tick.
The look catalogue is published as an immutable list (`models::echoes()` returns a shared pointer), so the
menu reads it every frame without copying it.

## Chat (`ui/chat.cpp`, `net/`)

CHAT packets (type 3: u32 message id, u8 length, UTF-8 text of at most 120 bytes after cleaning) are
relayed by the host like the others. UDP may drop one, so each message goes out in 3 consecutive send
cycles; receivers keep the last 16 ids per player and show a message once. Text is cleaned on both
sides (`protocol::cleanChatText`: no control characters, cut at a UTF-8 boundary). At most 5 messages
per player per 5 s are shown. The overlay opens the input line with `[ui] chat_key` (Enter by default; the key press that opens the line is not typed into it): key presses
then go to ImGui, while key releases, raw key releases and the mouse still reach the game (a key held
when the chat opened is released). Escape cancels, Enter sends. The last 8 lines show bottom-left
for 12 s, then fade (`[ui] show_chat`). The solo test does not echo your own message twice.

## Menus do not pause a session (`game/hooks.cpp`)

The game pauses through a counter object at `PamUISystem` +0x8c8: ints at +0x08 (pause requests) and
+0x0c (full pause requests), changed by +1/-1 in `UIPauseController_request` (`0x143b16210`) from the
UI pause entities. `PamUISystem`'s update (`0x143b16c50`) is the only caller of
`UIPauseController_applyToClient` (`0x143b16ae0`), which pauses or resumes the client from those counts.
In a session (hosting or connected) the hook sets both counts to 0 for the call to the original update and
afterwards adds the saved counts back, so requests made during the update are kept and the counts stay
right when the session ends: the game then pauses as normal.

## Go to a player (`game/teleport.cpp`)

The menu's *Go to* uses the game's own character teleport, `ServerCharacterEntity_teleport`
(0x143378830; what `ServerTeleportEntity_teleportPlayer` 0x143391430 calls): `(character, const
LinearTransform*)`, target read with MOVAPS (16-byte aligned). It stores the target (+0x410), moves the
physics controller (+0x60, vtbl+0x140, velocity zeroed via vtbl+0x168), sets the transform, sends
`CharacterToComponentsOnTeleportedMessage` to the components and marks the character dirty, so
the client follows. Sequence on the server tick: fade to black (350 ms, drawn by the overlay) ->
teleport onto the other player's spot, facing their way -> hold 1.2 s while the area streams in
(placed again if more than 2 m below the target, at most 3 times, only while it is still the same
character near the target: a death or respawn ends it) -> fade in. The fade also ends by itself if a
phase runs 4 s over (the server tick can stop while the game handles a death), so the screen never
stays black. The target's own state is not checked: a player who is dead may stand somewhere deadly. Before the first use
the function's bytes are checked; if they differ, *Go to* is off (logged), the rest of the mod runs.

## Positional voice (`game/mumble_link.cpp`)

Mumble's Link plugin reads the shared memory `MumbleLink` (`LinkedMem`, version 2). Each rendered frame
(only while the camera can be read) the mod writes the camera position, look direction (minus the
camera's forward row) and up as both avatar and camera, converted to Mumble's left-handed axes by
negating z, and increments `uiTick`. The context is `MECMP-<host session id>`, the same for everyone
in a session: the HELLO packet carries a host flag (u8 after the look; older builds omit it) so
joiners learn the host's session id even though relayed packets all arrive from the host's address.
Identity = the player's name. Turning it off writes `uiVersion = 0` and unmaps.
The menu's status line (`mumble::status()`): a `mumble.exe` process (Toolhelp snapshot, at most every 3 s,
only while the menu draws it) or a `MumbleLink` mapping that existed before we opened it counts as
Mumble found; "sending" means positions went out in the last second. Mumble writes nothing back, so
a real link cannot be confirmed from the game. The log says once when positions start going out.

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
- **Dedicated server** (`tools/server/mecmp_server.cpp`, rules in `server/relay.cpp`, docs/SERVER.md):
  relays like a host but is not a player. Its HELLO (about once a second, flags host | server, name =
  the server's name) gives clients the session id (Mumble context) and the name for the menu; it sends
  no STATE, so it never gets a ghost or a player-list entry. It binds each player id to the address
  that first used it, rate-limits per player and drops silent players after 10 s.
- **Server list** (`net/serverlist.cpp`, menu *Servers*): while the menu is open, its own socket and
  thread ask each saved server every 5 s with an INFO packet (type 5: an 80-byte question, a 44-byte
  answer with name, players, max players; the header echoes the question's seq for the round trip).
  INFO is not a session packet (not in `knownType`), and a server answers whatever the protocol, so
  the list can say "other version". Addresses are looked up on that thread.
- Packets: STATE 30 Hz and HELLO (name and look: u32, 0 = Faith, else the echo hash) ~1 Hz. Remote players are keyed by the random session id
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
  procedure. While open, mouse and key presses go to the menu (virtual cursor from raw input); key
  releases (WM_KEYUP and raw-input releases) still reach the game, so a key held when the menu
  opened does not stay held there.
- The chat key and the marker key do nothing while one of the game's own menus is up (pause menu,
  map; `hooks::gameMenuOpen`: the UI pause counts at `PamUISystem` +0x8c8 are above 0), so Enter
  can pick a menu item there.
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
