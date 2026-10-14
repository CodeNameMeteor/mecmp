# MECMP: Mirror's Edge Catalyst multiplayer

See other players running through the city with you. Every remote player appears as Faith (or
one of the game's ghost looks, as they pick), animated live from their own movement: running, jumping, climbing.
Free roam: one player hosts and friends join with a code, or everyone joins a dedicated server.

The mod does not touch the game's online services, Denuvo or the EA app. It talks to other
players over its own small UDP connection (port 47800 by default). Custom servers that only
change the server address (e.g. Beat Revival) keep working alongside it.

Status: public beta (0.9.0, see [CHANGELOG.md](CHANGELOG.md)). Ghosts are visual only (no collision).
Download the latest release from the [Releases](https://github.com/CodeNameMeteor/mecmp/releases) page.

## Use

1. Put `mecmp.dll` and `mecmp_inject.exe` in one folder. The first run creates `mecmp.ini` there.
2. Start the game, run `mecmp_inject.exe` (it says when the mod is loaded; `mecmp.log` says "ready"), load into free roam.

**In game, press INSERT, or click both sticks (LS+RS) on a gamepad,** for the MECMP menu: host a
session, paste a join code and join, set your name, see who is connected, pick your own look (Faith,
or the game's runner ghost; everyone else sees you that way). Other players' names float above their ghosts (*Names above players* in the menu turns
them off), and a short notice shows when someone joins or leaves (*Join/leave notices*). The player
list shows each player's connection: *good*, or the share of their updates that did not arrive. *Go to* next to a player
takes you to them (a short fade, then you are where they are). **Summon**: tap **G** and everyone gets a marker where you stand and a panel to hold **1** (go) or **2** (no); hold G to pull players in who allowed it (see the changelog and the *Controls* tab). Press **Enter** to
chat (not while the game's own pause menu or map is up): type a line and press Enter (Escape cancels); messages show bottom-left for a while. The menu works with mouse, keyboard or gamepad; while it is open the game does not see the
gamepad. The *Controls* tab changes the keys and buttons for the menu, chat and markers. Everything can also be set in
`mecmp.ini` (the menu saves to it).

The mod starts **offline**: nothing is sent and no router port is opened until you host, join or pick
a server in the menu. *Leave* goes offline again. *Start this session when the game starts*
(`[net] auto_start=1`) hosts, or joins the last session you joined, at launch.

**Hosting** (the menu's *Host a session*, or `join=` empty with `auto_start=1` in `mecmp.ini`): the mod asks your router to open UDP port 47800
(UPnP) and makes a **join code** like `2HMAS-W9ENR`. It is shown in the menu (*Copy* puts it on
the clipboard), saved in `join_code.txt` and written to `mecmp.log`. Send it to your friends. The host
forwards everyone's movement to everyone, so friends need nothing opened on their side.

**Joining**: paste the host's code in the menu and press *Join* (or set `join=2HMAS-W9ENR`).

**Servers**: a dedicated server (`mecmp_server`, runs without the game, for example on a VPS) is a
session that is always there. Type its address in the same field (`play.example.com`, with `:port`
when not 47800) and press *Join*; the menu shows *Server: <name>*. Or add it to the menu's *Servers*
list, which shows each saved server's name, players and ping (`[net] servers=` in `mecmp.ini`). Running one: [docs/SERVER.md](docs/SERVER.md).

If the host's router has UPnP turned off, forward UDP 47800 to the host's PC by hand. If the log
says the address is not reachable (carrier-grade NAT, common on mobile internet), let another
player host. Players on the same network can use the LAN code from the log.

Developer builds (CMake option `MECMP_DEBUG_KEYS`, on by default, off for release builds) also have:

| Key | Action |
|---|---|
| F7 | Spawn missing ghosts now / show them again / refresh their look |
| F12 | Hide all ghosts and pause automatic spawning (F7 resumes) |
| F9 | Write a status line to `mecmp.log` (with the ghost update's cost) |
| F10 | Developer: log the camera matrices (used to find the name tags' camera; needs a level) |

Versions: everyone in a session needs the same wire protocol (this version uses protocol 6). Packets
from a different protocol are ignored and the log says which version the other side runs.

While you are in a session, the game's own menus (pause menu, map) no longer pause the game: the
other players keep moving and you keep sending your position.

Solo test: *Solo test* in the menu, or `join=127.0.0.1:47800`. Your own packets come back and a
ghost copies your moves 3 m away along the world X axis (`loopback_offset`); it stays put while you
turn, so you can walk around it.

## Positional voice (Mumble)

MECMP has no voice chat of its own; it tells [Mumble](https://www.mumble.info) where you are, so
players hear each other from the direction of their ghost, quieter with distance.

1. Everyone installs Mumble and joins the same Mumble server and channel.
2. In Mumble: *Settings > Audio Output > Positional Audio* on, and *Settings > Plugins > Link* enabled
   (with *Link to Game and Transmit Position* on).
3. Play in the same MECMP session. Mumble's log shows a "linked" line when it picks the game up.

The MECMP menu shows the state under *Positional voice (Mumble)*: Mumble not running, waiting for a
level, waiting for a session, or *Sending to Mumble* (hover it for the Mumble settings). Mumble does not
answer the game, so this shows that MECMP is doing its part; whether Mumble uses it shows in Mumble.
To test it, two players in the same session walk apart: the other voice should move and fade.

Only players in the same MECMP session are positioned for each other (outside the game, or in another
session, Mumble falls back to normal voice). Turn it off with *Positional voice (Mumble)* in the menu or
`[voice] mumble_link=0`.

## Security and privacy

- Traffic is plain UDP, with no encryption and no login. Anyone who can reach the host's port can
  send packets that look like another player. Only host sessions with people you trust.
- The join code contains the host's public IP address and port. Anyone with the code can join.
- Hosting asks the router to open the port with UPnP, for one hour at a time (renewed while
  hosting). The mapping is removed when the game closes. Routers that only allow permanent
  mappings are not used; forward the port by hand there.
- `mecmp.log` lists other players' IP addresses; check it before you share it.

## Build

CMake, C++17, x64. Visual Studio 2022: *File → Open → Folder*, pick the repo, build
`x64-Release`. Or cross-compile with llvm-mingw:

```
cmake -B build -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Outputs: `mecmp.dll` (the mod), `mecmp_inject.exe` (loader), `version.dll` (optional proxy loader),
`mecmp_server.exe` (dedicated server). On Linux, the same CMake build makes only `mecmp_server`.

## Layout

```
src/app     startup, per-tick update, hotkeys, crash logging
src/core    log, config (mecmp.ini), safe memory access, engine type list
src/game    engine addresses, hooks, level state, ghosts, ghost models, local player capture
src/net     UDP peer-to-peer link, wire protocol, UPnP, join codes
src/server  dedicated server relay rules (portable, tested on Linux)
src/ui      in-game overlay (Dear ImGui on the game's D3D11 swap chain) and the menu
third_party Dear ImGui 1.91.9 (MIT)
tools       injector, version.dll proxy, dedicated server program
docs        how it works and the roadmap
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/ROADMAP.md](docs/ROADMAP.md).

## Licence

MIT, see [LICENSE](LICENSE). Dear ImGui is MIT too ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

## Releases

Push a tag `v<version>` matching `kVersion` in `src/app/mod.h` (e.g. `v0.9.0`). The release workflow
builds the Windows package (mod, injector and server, without the developer keys) and the Linux server,
and opens a **draft** GitHub release with them and that version's CHANGELOG section. Check it and
publish it by hand.
