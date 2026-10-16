# Changelog

## Unreleased

### Runs
- **No teleports in or out of a run, from anywhere.** The players panel already refused while you are in a
  time trial or dash; now the menu's *Go to* / *Ask to go*, *Summon* buttons, the "Go back?" hold after a summon
  and an accepted "may I come?" do too (greyed out with a reason, or a notice). The rule sits in the teleport
  itself, so a path added later cannot skip it.
- **A Beat L.E. (ReachThis) is not a run.** Opening one counted as starting a time trial: after closing it you
  stayed "in a time trial" (no teleports, shown above your head) until another run or 30 minutes. Reaching one
  is not announced as a time either.

### Faster ghosts after a load
- **Ghosts come back much sooner after a level load or a checkpoint reload.** Ghosts were made one every 2 s,
  after a 3 s wait, so five players took about 13 s. Now the next ghost is made as soon as the previous one is
  ready (usually a fraction of a second), and the first one 1.5 s after your character appears. If a ghost's
  parts never arrive, it still waits 2 s, as before.

### Pause
- **The pause menu and the map pause the game again**, in a session too. (Since 0.9.0 they kept the world
  running while you were in a session; players wanted the pause back.)

### Fixes
- **Ghosts no longer freeze when someone joins.** A new ghost spawner the game creates (moving around the
  city) could take over from the one the ghosts came from. The next spawn, from that spawner, failed, and the
  mod then took every existing ghost for destroyed and stopped moving them: they froze where they were, and
  the joining player never appeared ("the spawner returned no character" every 2 s). Now the chosen spawner
  stays, each ghost is recognised by its own data, a failed spawn changes nothing for the others, and a
  spawner that gives no character three times in a row is replaced by another.

### Controls
- **Any key can be a bind.** Every key on the keyboard can now be bound in the *Controls* tab: arrows, the
  number pad, punctuation, F13–F24, Tab, Space, Shift/Ctrl/Alt, and keys only some layouts have (a UK
  keyboard's `#`, say), not only a fixed list. Keys show their own name on your layout ("Page Up", "Num 4").
  Escape, the Windows keys and Print Screen stay free.
- **The menu key can be anything the other binds can,** including a letter, a digit or the middle or side
  mouse buttons. A typing key used as the menu key still types in the chat and in the menu's text boxes.
- F10 and Alt work as binds (Windows sends them as system keys, which the mod did not listen to).

### Player sounds (by ReGlitched)
- **Hear other players.** Their footsteps, hand contacts, landings, breathing, effort grunts and combat sounds
  play through the game's own audio, from where they are. *Players* tab: *Other players' game sounds* and a
  distance (80 m by default). **Off by default** until it has been play-tested; turn it on to try it. Everyone
  needs this version, and a dedicated server must be updated too. See `docs/PLAYER_AUDIO.md`.
- **Stretched bodies are cut short.** Hosts and dedicated servers shorten limbs longer than `bone_reach`
  (1 m by default, 0.5 to 2) before forwarding a pose, and every player also caps them at 2 m.

### Hosting without port forwarding
- **Hole punching.** A host registers with a dedicated server ([net] `rendezvous` in mecmp.ini, the community
  server by default); when someone joins with the host's code, the server tells each of them the other's address
  and both send at once, which most home routers let through. Only that introduction goes through the server:
  the game's traffic goes straight between the players, as before. When it does not work (some mobile and
  carrier networks), joining is exactly as it was: the host needs UDP 47800 forwarded (or UPnP), or everyone
  joins a dedicated server, and the log now says so. Host, joiner and the rendezvous server all need this
  version (new packet type 14; older ones ignore it). `rendezvous=` (empty) turns it off.
- The server's `status` command counts the hosts registered for hole punching and the introductions made.
- **`[net] upnp=0`** stops the mod from asking the router to open the port (to test hole punching on its own,
  or if you would rather it did not).

### Discord
- **Finished runs are cleaned up with the session.** When the server empties, its dash and time-trial posts are
  deleted along with the joins and leaves (they used to stay). `webhook_keep_results=1` keeps them.
- **Nothing is left behind by a restart.** The ids of the posts still to delete are kept in
  `webhook_activity_file` (`webhook_activity.txt` next to the log), so a server that was stopped, restarted or
  crashed mid-session deletes them at its next start.

### Fixes
- **Ghost spawner choice is steady during time trials.** A trial brings a ghost spawner that scores the same
  as the city's, and the mod switched between the two every few seconds ("ghost spawner ... chosen" over and
  over in the log); a ghost made from the trial's one would go when the trial ends. The current spawner now
  stays unless another is clearly better.
- **Fixed a crash with *Players on the map*** in some builds (MSVC Release): the position handed to the game's
  friend-marker code was not 16-byte aligned, which the game requires there.
- **Time trials with names outside plain ASCII** (accents, other alphabets) are named in the finish notice and on
  Discord; such a name used to be left out.
- **The overlay hands the game back its render targets** after drawing, skips the game's test presents (which
  show nothing), finds the fonts wherever Windows is installed (not only on C:), and stores the game's window
  procedure before taking over its input (a message arriving in between was lost).
- **No hitch while the menu shows the voice status**: checking whether Mumble runs (a list of every process)
  moved off the render thread.

### Markers
- **Colors of their own per marker.** Each slot of a marker wheel has two swatches next to its title: the
  marker's own color and secondary color. A faded swatch follows the main color; right-click a set one to
  go back to it. The wheel, the menu and the marker everyone sees use them (`wheelN_color_S` /
  `wheelN_accent_S` in mecmp.ini). A bookmark of such a marker keeps its colors.
- **Two resets**: *Reset colors* puts main and secondary back to red and white and leaves own colors
  alone; *Reset own colors* sends every marker back to the main ones.
- **Red is the default color** (main red, secondary white) for new settings; existing mecmp.ini files keep
  their colors.
- **Two-tone icons.** Nearly every icon now has a part in the secondary color (a half of the arrows, the
  exclamation mark, the inner shape of the star, heart and shield, the ends (half a square) of the plus,
  cross, check, pause and minus, the three dots of the speech bubble, the zipline's anchors, the door's sign ...). The
  eye's frame is a quarter thicker, the target's center a quarter bigger, the shield's top dips in the
  middle, the climb icon is a fifth wider. The ring and the dot are no longer offered in the icon list;
  markers and ini files that have them keep them.
- **Fixed: a marker key set to the middle button (`ping_key=MOUSE3`) did nothing after the game started**
  until it was set again in the menu.
- **A sharper question mark and a new exclamation mark icon** (a bar with the dot in the secondary color,
  `exclaim` in mecmp.ini). The ready-made wheel 2 now has "What?" (the circle with the question mark) and
  "Help!" (the exclamation mark); "Nice!" moved to wheel 3. Existing wheels are kept; *Reset this wheel*
  brings the new ones.
- **Slots without a title look unset**: in the menu their icon and colors are at half opacity, and in the
  open wheel the wedge, digit and icon are dimmer (they still place an icon-only marker). The wheel is
  drawn 15% bigger.
- **Quick answers show under the marker**, for everyone: the answer's icon in a row of up to three, then
  "and N more". A new answer comes in on the right and the row stays centered. Answers have colors of
  their own like markers (`react_color_N` / `react_accent_N`). Old versions ignore the added bytes and
  show the usual notice; an answer from an old version shows as a white speech bubble.
- **The Markers tab is sorted** into Markers, Colors, Wheels and Manage markers (placed markers,
  bookmarks, the others' markers and *Clear my markers*).
- **Fixed: an answer to a marker that was replaced meanwhile** showed under the new marker in that cell; and
  answering many players' markers could drop an answer to yours (the flood limit counted all of them).

### Players panel
- **Hold T** (`players_key`; `players_pad`, DOWN+RB on a gamepad) for a panel with the other players' names in
  the middle of the screen, nine to a page. Right-click a name to **go to** that player, click names to
  **pick** them (they turn red); 1-9 go to that slot, Q / E turn the page. Letting go of the key only closes
  the panel, so a stray release never teleports anyone. On a gamepad the right stick moves the pointer, RT
  clicks, LT goes to, LB turns the page.
- **Summon from the panel, for everyone**: the button below is *Summon picked* (or *Summon everyone* with
  nobody picked); while your summon waits for answers the same button withdraws it. The host (or a member the
  host allows) also gets a *Hard summon* box that makes it pull players in. The menu's Players tab got a
  *Withdraw summon* button too.
- **The summon key (G, `summon_key`) is gone**: the panel does what it did. The accept and decline keys stay.
- **Summon again after 5 s** instead of 10 s.
- **The Controls tab folds**: Markers, Players panel and Summon answers start open; Quick answers, Chat, Overlay,
  Menu and Gamepad are closed, and *Advanced* holds how the wheel opens and the pointer speeds. The Markers and
  Display tabs fold the same way; their *Advanced* holds *Flash markers when placed* and the interface size.
- **The mod's mouse cursor follows the Windows pointer speed** (the menu, the panel and the marker wheels' pointer;
  it used the mouse's raw counts, much too fast with a high-DPI mouse), plus a *Menu mouse speed* setting
  (`menu_mouse_speed`).
- **Runs**: a player in a time trial or a dash is marked *in a run*. A Go to on them waits and goes once their
  run is over (right-click them again to call it off). A summon is sent; on their side it waits, unseen, and its
  time starts when the run ends. While you are in a run yourself, the panel does not teleport or summon.
- **Ask me before others go to me** (You tab, Privacy; `ask_before_go_to` in [player]): a Go to on you then asks
  first, shown like a summon (hold the accept key for yes, the decline key for no). The menu's Go to button
  asks too. New VISIT packet (type 13, no protocol bump): older versions never ask and are never asked; a
  dedicated server must be updated to pass it on.

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
- Own UDP protocol (version 7), about 4–5 KB/s per player. Every player in a session needs the same
  version.
- No memory scans in normal play: what the mod needs is noted as the game builds each level.

### Also in this release
Summon, players on the game's map, finished runs and live run clocks, Discord posts from servers, quick
answers, and more:

### Controls
- **The middle button can be bound** to the marker, quick answer, other-way, hide and wheel keys and to the
  summon keys (it was only offered for "clear markers"). While it is bound to one of them, holding it in the
  open wheel no longer clears your markers.
- **Hiding and showing the overlay take a one second hold** of the hide key (a bar fills at the top), so a
  stray press does nothing.
- **Poses go out at 30 Hz** (they went out about 21 times a second, because the send loop woke on the
  system's 15.6 ms timer ticks). The loop now asks for a 1 ms tick while the session runs and sends from a
  deadline on the high-resolution clock.
- **Summon texts say "summon"** everywhere ("Nobody to summon", "Summoning everyone to you", ...).
- **The "Go back?" bar can be hidden**: hold the summon accept key (1) for a second. Going back is then
  no longer offered for that summon.

### Menu
- **Six tabs**: Session (with the host's settings while hosting), Players (list, Go to, summon, show /
  hide ghosts, Mumble voice), You (your look; privacy: *Share my EA profile*, *Allow being pulled in*),
  Markers, Display (interface size, what is shown, hide all) and Controls (keys and buttons only).
  No setting changed; some moved.

#### Seeing what players do
### Dedicated server
- **`reset` console command.** Drops every player (they rejoin on their own when their game next asks), clears the
  counters and reads `mecmp_server.ini` again, so a changed name, player limit or motd takes effect without a
  restart. Bans are kept. The port, log and bans files and the Discord webhook still need a restart, and the
  command says so.

### Seeing what players do
- **A run's clock above the runner.** While a player is in a dash (or a delivery, GridNode, MetaGrid run) or a
  time trial, their name tag shows it with a running clock: "Dash: The View  12.4". It clears when the run is
  finished or cancelled. The clock is counted from when the player's game started the run (the countdown
  may be in it); it is for watching, not a result.
- **"typing..." above a player** whose chat box is open.

#### Injector
- **The injector's window closes by itself** 10 seconds after a successful injection (a key closes it at
  once). After a failure it still waits for a key, so the error can be read.

#### Finished runs
- **When someone finishes a dash, everyone sees it**: "Meteor finished the dash The View in 1:23.45
  (new record, 2.10 faster)". Deliveries, GridNodes and the MetaGrid too; billboard hacks are not timed
  and are not announced. The time is what the player's own game reports.

#### Discord
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

#### Players on the map
- **The session's players show on the game's map**, the way the game showed online friends: a marker
  with their Runner Tag that follows them, with their profile name on hover. It uses the game's own
  friend markers (10 per level). Needs online services (Beat Revival): the Runner Tag is loaded from
  the online profile. *Players on the map* and *Share my EA profile* in the menu (`[ui] players_on_map`,
  `[player] share_profile`): sharing is off by default; when on, it sends your EA online id and profile
  name to your session, so you show on their maps.

#### Performance
- **Ghosts cost ~50x less per frame.** Writing a ghost's pose each frame took ~0.3 ms, almost all of it
  in Windows' WriteProcessMemory, which flushes the CPU's instruction cache on every call. The mod now
  uses the kernel call underneath it (just as safe if the memory is gone): ~13 us per pose write
  instead of ~350 us. Other hot paths read game
  memory in fewer calls (one read per object instead of one per field; the game's own type data and
  vtables are read directly). The whole mod now costs ~15 ms per second of game time with one ghost,
  most of it the overlay.
- **F9 logs a "perf:" line** (developer builds): time per call, calls per second and memory calls per
  call for each part of the mod (tick, capture, ghosts, render pose, UI check, overlay).

#### Changes
- **The solo test is a developer tool now.** Release builds have no *Solo test* button and ignore
  their own packets (joining yourself shows nobody). Developer builds keep it. A server started with
  `--mirror` still mirrors you, with any build.

#### Fixed since the test builds
- **A player who went to the main menu or a loading screen stayed as a frozen ghost** for everyone else,
  listed as live (and *Go to* took you to where they had been). Their game kept sending its last position;
  now it sends nothing until it is in a level again, and their ghost is hidden as for anyone who stops.
- **Joining a server by its name no longer freezes the game** while the name is looked up (seconds with a
  slow or unreachable DNS server). The menu shows "Starting the session..." meanwhile.
- **Names and titles in any language are kept in mecmp.ini.** The file was read and written in the system's
  code page, so a name outside it (Japanese on a Western PC) came back as "??", and a file saved as UTF-8
  (Notepad) was misread. mecmp.ini is now converted to Unicode (UTF-16) when it is read; your settings stay.
- **The server starts with `--config` pointing at a file that does not exist yet** (the systemd setup in
  docs/SERVER.md does that): it writes the commented default there, as it does without `--config`. Before, it
  stopped at once with "cannot read".
- **A player who came back from a mission was never seen again**, not even his name tag (and "Go to" said
  the player character was not available). When the mod missed the new level's player spawner, the scan
  that finds it only ran if the player character already existed, which it did not without the spawner.
  It now runs 20 s into a level whenever something is missing.
- **Ghosts froze half a second after they appeared** when the game was started with the mod before the level
  (the usual way), and the mod took the wrong spawner for them: the newest ghost spawner it saw, which is
  not always the city's. It now looks at all of them and takes the one with its own data that holds the
  game's own city ghost, before it makes a ghost (and again whenever the game builds a new one).
- **Other players' ghosts no longer disappear for good** after the game rebuilds the city (a district
  streaming out and in). The mod kept pointing at the ghost spawner the game had destroyed, so ghosts that
  were taken away were never made again, while the log still said the spawner was found. A scan now
  replaces a dead spawner, and a level may be scanned up to three times (not once) when something is missing.
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

#### Marker wheel
- **Instant wheel** (Controls tab): the wheel can open the moment the key goes down instead of after a
  short hold. A very short tap that never moves the pointer still places your marker.
- **Easier to aim at a low mouse sensitivity.** The pointer is kept close to the middle, so turning to
  the opposite wedge is a short move; the dead zone is smaller, there is a *Pointer speed* setting
  (0.1 to 3), and
  the right mouse button cancels. The wheel looks as it did.
- **Slots per wheel (2 to 9).** Each wheel shows the first N of its nine slots; the others are kept, so
  raising N again brings them back. Digits, wedges and the tap slot follow.
- **Wheel names.** Each wheel can have its own title.

#### Markers
- Markers are drawn 15% smaller until your crosshair is on one, then they grow to 10% over their old size.
- **Double tap the clear key** (X now, was H) to remove just the marker of yours you look at (the one
  at your feet within 1.5 m when markers go where you stand). Holding it still removes all of them.
- Titles of markers that would cover each other, or another marker's icon, stack upwards (only up,
  so they do not jump about), with a thin line back to their marker.

#### Keys
- **Mouse 4 / Mouse 5** can be bound to the marker, quick answer and the new keys below (nothing is
  bound to them by default).
- **Place the other way** (B): a marker where you stand if markers go where you look, and the other
  way round, without opening the settings.
- **Hide the overlay** (Y, or the checkbox on the Markers tab): names, markers, arrows, notices and
  chat lines are not drawn, for a screenshot or a recording.
- **Previous / next wheel** (Q / E) while the wheel is open. The arrows and the mouse wheel still work.
  While the marker key is down the game does not see the wheel's keys (digits, arrows, Q, E).

#### Quick answers
- New **react key** (C by default): look at another player's marker, tap to send your last quick
  answer, hold for the answers wheel. The owner sees `Name reacted "On my way" to your "Here" marker`.

#### Summon
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

#### Gamepad
- **PlayStation names** for the buttons in hints (Controls tab, `pad_labels`). Settings keep Xbox names.
- Tool bag now has a tooltip like MagRope and GridLink.

### Known issues
- Echo ghosts use your own echo style (see above).
- In a level the mod was injected into mid-way, a ghost may stay invisible; load a level or a
  checkpoint (or inject at the main menu).
- Ghosts have no collision. Traffic is not encrypted: only play with people and servers you trust.

### Thanks
TheNollz (markers, markers v2, tabbed menu) and Zak / Col-Vargas (ghost skins and the session pause).
