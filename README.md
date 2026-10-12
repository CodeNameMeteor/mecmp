# MECMP: Mirror's Edge Catalyst multiplayer

See other players running through the city with you. Every remote player appears as Faith (or
one of the game's ghost looks, as they pick), animated live from their own movement: running, jumping, climbing.
Free roam, peer to peer, no custom server needed.

The mod does not touch the game's online services, Denuvo or the EA app. It talks to other
players over its own small UDP connection (port 47800 by default). Custom servers that only
change the server address (e.g. Beat Revival) keep working alongside it.

Status: experimental. Ghosts are visual only (no collision).

## Use

1. Put `mecmp.dll` and `mecmp_inject.exe` in one folder. The first run creates `mecmp.ini` there.
2. Start the game, run `mecmp_inject.exe` (two beeps = the mod is active), load into free roam.

**In game, press INSERT, or click both sticks (LS+RS) on a gamepad,** for the MECMP menu: host a
session, paste a join code and join, set your name, see who is connected, pick your own look (Faith,
or any of the game's ghost styles in any colour; everyone else sees you that way). Other players' names float above their ghosts (*Names above players* in the menu turns
them off), and a short notice shows when someone joins or leaves (*Join/leave notices*). The player
list shows each player's connection: *good*, or the share of their updates that did not arrive. The menu works with mouse, keyboard or gamepad; while it is open the game does not see the
gamepad. Both menu buttons can be changed under *Controls*. Everything can also be set in
`mecmp.ini` (the menu saves to it).

**Hosting** (the menu's *Host a session*, or `join=` empty in `mecmp.ini`): the mod asks your router to open UDP port 47800
(UPnP) and makes a **join code** like `2HMAS-W9ENR`. It is shown in the menu (*Copy* puts it on
the clipboard), saved in `join_code.txt` and written to `mecmp.log`. Send it to your friends. The host
forwards everyone's movement to everyone, so friends need nothing opened on their side.

**Joining**: paste the host's code in the menu and press *Join* (or set `join=2HMAS-W9ENR`).

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

Versions: a player on this version and one on an older version can still play together. An older
version does not know player looks and shows every player with its own `model=` setting, so the looks
you picked may not show there.

Solo test: *Solo test* in the menu, or `join=127.0.0.1:47800`. Your own packets come back and a
ghost copies your moves 3 m away along the world X axis (`loopback_offset`); it stays put while you
turn, so you can walk around it.

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

Outputs: `mecmp.dll` (the mod), `mecmp_inject.exe` (loader), `version.dll` (optional proxy loader).

## Layout

```
src/app     startup, per-tick update, hotkeys, crash logging
src/core    log, config (mecmp.ini), safe memory access, engine type list
src/game    engine addresses, hooks, level state, ghosts, ghost models, local player capture
src/net     UDP peer-to-peer link, wire protocol, UPnP, join codes
src/ui      in-game overlay (Dear ImGui on the game's D3D11 swap chain) and the menu
third_party Dear ImGui 1.91.9 (MIT)
tools       injector and version.dll proxy
docs        how it works and the roadmap
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/ROADMAP.md](docs/ROADMAP.md).
