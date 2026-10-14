# Changelog

## Unreleased

### Fixes
- **Hosting is harder to flood.** A host accepts at most 4 players from one IP address, refuses a
  player who claims another player's session id, and limits each player to 80 packets a second.
  Before, one machine could take every slot of a session, and another player could send as someone else.

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
