# Changelog

## Unreleased

### Finished runs
- **When someone finishes a dash, everyone sees it**: "Meteor finished the dash The View in 1:23.45
  (new record, 2.10 faster)". Deliveries, GridNodes and the MetaGrid too; billboard hacks are not timed
  and are not announced. The time is what the player's own game reports.

### Discord
- **Closing the server's window sets the live message to offline.** Closing the console window (its X), logging
  off or shutting Windows down used to end the server at once, so Discord kept saying it was online; only
  Ctrl+C or `quit` did it. The server now shuts down properly in the few seconds Windows allows, and the
  "offline" edit goes out first. On Linux, closing the terminal (SIGHUP) does the same.
- **A live "who's online" message in Discord**: one message, edited as players join and leave, with the
  player list and the address to join; "offline" when the server stops (`webhook_live`).
- **Servers can post to Discord.** Set `webhook_url` in `mecmp_server.ini` and the server posts when it
  goes online (with the address to join) or offline, and when players join and leave, with the player
  count, and finished runs with their times (`webhook_results`). Posts are batched and rate-limit aware;
  names cannot ping anyone. See docs/SERVER.md.

### Players on the map
- **The session's players show on the game's map**, the way the game showed online friends: a marker
  with their Runner Tag that follows them, with their profile name on hover. It uses the game's own
  friend markers (10 per level). Needs online services (Beat Revival): the Runner Tag is loaded from
  the online profile. *Players on the map* and *Share my EA profile* in the menu (`[ui] players_on_map`,
  `[player] share_profile`): sharing is off by default; when on, it sends your EA online id and profile
  name to your session, so you show on their maps.

### Performance
- **Ghosts cost ~50x less per frame.** Writing a ghost's pose each frame took ~0.3 ms, almost all of it
  in Windows' WriteProcessMemory, which flushes the CPU's instruction cache on every call. The mod now
  uses the kernel call underneath it (just as safe if the memory is gone): ~13 us per pose write
  instead of ~350 us. Other hot paths read game
  memory in fewer calls (one read per object instead of one per field; the game's own type data and
  vtables are read directly). The whole mod now costs ~15 ms per second of game time with one ghost,
  most of it the overlay.
- **F9 logs a "perf:" line** (developer builds): time per call, calls per second and memory calls per
  call for each part of the mod (tick, capture, ghosts, render pose, UI check, overlay).

### Changes
- **The solo test is a developer tool now.** Release builds have no *Solo test* button and ignore
  their own packets (joining yourself shows nobody). Developer builds keep it. A server started with
  `--mirror` still mirrors you, with any build.

### Fixes
- **The overlay follows the game window's size again, and has a size setting.** The font was sized once,
  from the window as it was when the overlay started. The game starts in a 1280x720 window and then goes
  full screen, so on a 1440p or 4K screen the menu, markers and the wheel stayed at 720p size (the wheel
  was about 17% of the screen's height, the labels a few pixels). The size now follows the window's height
  all the time. *Interface size* (menu, Players tab; `[ui] ui_scale`, 0.5..2) scales it further.

- **Joining works with a server on the same PC.** A mecmp_server (or a second game) on this PC holds
  UDP 47800, so the mod could not start at all ("cannot use UDP port 47800"). A joiner now uses any free
  port when its own is taken; hosting still needs the configured port, and the message now says what
  is using it and how to change it.
- **Teleports land on the floor.** *Go to*, a summon and *go back* used to drop the player through
  thin floors or into a city that had not streamed in yet. The screen now stays black while a ray looks
  for the ground under the target; the player is held there, set down on the ground it finds and put
  back if falling, and the fade ends once they stand still. With no ground after 8 s they go back to
  where they started.
- **Player arrows fade with distance** like markers (50 m to 300 m), so far players, and the map
  screen, no longer show arrows thousands of metres away.
- **A joiner listens only to its host.** Everything in a session reaches a joiner through the host (or the
  server), so packets from any other address are now ignored. Before, someone who knew a player's address
  could send it chat, markers or a summon that appeared to come from the host.
- **The injector finds the game, not the activation window.** Starting the game first runs an
  activation process that is also called MirrorsEdgeCatalyst.exe; an injector started early loaded the
  mod into that one. It now waits until the game itself is running (the engine is loaded and its window
  is up), so it can be started before the game. Started by double-click, its window stays open until a
  key is pressed, so the result can be read.
- **"Your Echo" works with a session running.** In the main menu the mod no longer spawns ghosts or
  stops the menus from pausing. The preview echo used to vanish, and a ghost took its place.
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

### Summon
- **Call players to where you stand.** Tap **G** to call everyone, or tick players in the Players tab
  and press *Summon selected*. Each player sees a *Summon point* marker at your spot and a panel at the
  top: hold **1** for a second to go there, hold **2** to say no (nothing is done by itself; the call
  closes after a minute). On a gamepad: D-pad left / right, hidden from the game while the panel is up.
- **You hear back.** One player called: `Name accepted / declined your summon`. Several (or everyone):
  `Summon: 3 of 4 teleported`. Hold **G** for a second while your call is open to withdraw it; the
  markers disappear for everyone.
- **Go back.** After a summon took you somewhere, hold **2** for 3 seconds within a minute and you are
  where you stood.
- **Hard summon** (hold **G** for a second, or *Pull in* in the Players tab): the players who allowed
  it are taken to you after a 5 second countdown they can cancel (hold **2**) or skip (hold **1**). Off
  unless the player ticks *Allow being pulled in* (Controls tab, `allow_forced_summon`). Only the host
  can pull players in; the host's *Members may pull players in* (`members_may_hard_summon`) lets members
  do it too. Nobody can vouch for themselves: the host replaces any vouch written into a packet, and
  behind a dedicated server nobody is trusted at all (there a hard summon only asks).
- New keys on the Controls tab: `summon_key`, `summon_accept_key`, `summon_decline_key` (digits are
  allowed now) and `summon_accept_pad` / `summon_decline_pad`; `hard_summon_grace_ms` sets the countdown.
- Needs the new packet types 7 and 8 (protocol number unchanged), so a dedicated server must be
  updated as well; older versions ignore them.

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
- In a level the mod was injected into mid-way, a ghost may stay invisible; load a level or a
  checkpoint (or inject at the main menu).
- Ghosts have no collision. Traffic is not encrypted: only play with people and servers you trust.

### Thanks
TheNollz (markers, markers v2, tabbed menu) and Zak / Col-Vargas (ghost skins and the session pause).
