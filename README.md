# MECMP: Mirror's Edge Catalyst multiplayer

See other players running through the city with you. Every remote player appears as Faith (or
another model you pick), animated live from their own movement: running, jumping, climbing.
Free roam, peer to peer, no custom server needed.

The mod does not touch the game's online services, Denuvo or the EA app. It talks to other
players over its own small UDP connection (port 47800 by default). Custom servers that only
change the server address (e.g. Beat Revival) keep working alongside it.

Status: experimental. Ghosts are visual only (no collision).

## Use

1. Put `mecmp.dll`, `mecmp_inject.exe` and `mecmp.ini` in one folder.
2. In `mecmp.ini` set `peer=` to the other player's address (`ipconfig` → IPv4 Address), e.g.
   `peer=192.168.0.144:47800`. One side may leave it empty and wait to be contacted.
   Different houses: use Tailscale/ZeroTier, or forward UDP 47800 (built-in joining is next on the roadmap).
3. Start the game, run `mecmp_inject.exe` (two beeps = the mod is active), load into free roam.
   Other players' ghosts appear a few seconds after their packets arrive.

| Key | Action |
|---|---|
| F7 | Spawn missing ghosts now / show them again / refresh their look |
| F8 | Next ghost model (Faith, the red time-trial ghost, enemies, ...) |
| F12 | Hide all ghosts and pause automatic spawning (F7 resumes) |
| F9 | Write a status line to `mecmp.log` |

Solo test: leave `peer=127.0.0.1:47800`. Your own packets come back and a ghost mirrors you
3 m to your side.

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
src/net     UDP peer-to-peer link and wire protocol
tools       injector and version.dll proxy
docs        how it works and the roadmap
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/ROADMAP.md](docs/ROADMAP.md).
