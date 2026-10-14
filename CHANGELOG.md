# Changelog

## Unreleased

### Fixes
- **Hosting is harder to flood.** A host accepts at most 4 players from one IP address, refuses a
  player who claims another player's session id, and limits each player to 80 packets a second.
  Before, one machine could take every slot of a session, and another player could send as someone else.

### Marker wheel
- **Instant wheel** (Controls tab): the wheel can open the moment the key goes down instead of after a
  short hold. A very short tap that never moves the pointer still places your marker.
- **Easier to aim at a low mouse sensitivity.** The pointer is kept close to the middle, so turning to
  the opposite wedge is a short move; the dead zone is smaller, there is a *Pointer speed* setting
  (0.1 to 3), and
  the right mouse button cancels. The wheel looks as it did.
- **Slots per wheel (2 to 9).** Each wheel shows the first N of its nine slots; the others are kept, so
  raising N again brings them back. Digits, wedges and the tap slot follow.
- **Wheel names.** Each wheel can have its own title.

### Markers
- Markers are drawn 15% smaller until your crosshair is on one, then they grow to 10% over their old size.
- **Double tap the clear key** (X now, was H) to remove just the marker of yours you look at (the one
  at your feet within 1.5 m when markers go where you stand). Holding it still removes all of them.
- Titles of markers that would cover each other, or another marker's icon, stack upwards (only up,
  so they do not jump about), with a thin line back to their marker.

### Keys
- **Mouse 4 / Mouse 5** can be bound to the marker, quick answer and the new keys below (nothing is
  bound to them by default).
- **Place the other way** (B): a marker where you stand if markers go where you look, and the other
  way round, without opening the settings.
- **Hide the overlay** (Y, or the checkbox on the Markers tab): names, markers, arrows, notices and
  chat lines are not drawn, for a screenshot or a recording.
- **Previous / next wheel** (Q / E) while the wheel is open. The arrows and the mouse wheel still work.
  While the marker key is down the game does not see the wheel's keys (digits, arrows, Q, E).

### Quick answers
- New **react key** (C by default): look at another player's marker, tap to send your last quick
  answer, hold for the answers wheel. The owner sees `Name reacted "On my way" to your "Here" marker`.
  Needs the new packet type, so a dedicated server must be updated as well.

### Gamepad
- **PlayStation names** for the buttons in hints (Controls tab, `pad_labels`). Settings keep Xbox names.
- Tool bag now has a tooltip like MagRope and GridLink.

## 0.9.0: first public beta

Run through the city of Glass with other players. Each player shows up as a ghost that copies their
movement live: running, climbing, wall-running.

### Playing together
- **Host or join** from the in-game menu (INSERT, or both sticks on a gamepad). A host gets a short
  **join code** to send to friends; the mod opens the port on the router (UPnP) when it can.
- **Dedicated servers.** `mecmp_server` (Windows or Linux, no game needed) runs an always-on room
  with bans, a welcome message and per-address limits. The menu's **server list** shows each
  saved server's name, players and ping. See `docs/SERVER.md`.
- The mod starts **offline**. Nothing is sent and no port is opened until you host, join or pick a
  server; *Leave* goes offline again.
- **Go to** a player: a short fade and you are where they are.
- Player list with connection quality, and join/leave notices.

### Seeing each other
- Ghosts look like **Faith** or a **runner echo**; each player picks their own look. Echo ghosts show
  in your own "Your Echo" style and colour: the game only has your own echo style loaded in free roam.
- **Names** above players, and **arrows** at the screen's edge towards players you cannot see.
- Smooth motion: poses are written on the game's own thread and blended between updates.
- While you are in a session, the game's pause menu and map no longer stop the world.

### Talking
- **Text chat** (Enter by default).
- **Positional voice** through Mumble's Link plugin. The menu shows whether positions are reaching
  Mumble.
- **Markers:** wheels of 33 icons with titles, several markers at once, lifetimes, bookmarks, and an
  edge-of-screen arrow for markers out of view.

### Menu
- Tabs: Session (session and servers), Players (list, your look, other players), Markers, and
  Controls (every key and gamepad button for the menu, chat and markers).
- Mouse, keyboard or gamepad.

### Under the hood
- The mod never patches game code. It only swaps pointers in vtables and creator tables, and leaves
  Denuvo, the EA app and the game's online services alone (Beat Revival keeps working).
- Own UDP protocol (version 6), about 4–5 KB/s per player. Every player in a session needs the same
  version.
- No memory scans in normal play: what the mod needs is noted as the game builds each level.

### Known issues
- Echo ghosts use your own echo style (see above).
- With the mod loaded, the game's "Your Echo" preview can appear and vanish at once.
- In a level the mod was injected into mid-way, a ghost may stay invisible; load a level or a
  checkpoint (or inject at the main menu).
- Ghosts have no collision. Traffic is not encrypted: only play with people and servers you trust.

### Thanks
TheNollz (markers, markers v2, tabbed menu) and Zak / Col-Vargas (ghost skins and the session pause).
