# Door synchronization

Approved scope: ordinary free-roam doors, a saved per-player synchronization switch,
an Insert-menu open button and an optional keyboard binding. Branch:
`yamixdev/sync-nearby-doors`, based on upstream `pose-sync-fixes` at
`5a5211ec5ce82dcb3457389e6212352395c506bd` (PR #98).

## Behavior

- Observe natural opening as well as opening requested by the menu or hotkey.
- Apply remote opening through the game's ANT Boolean setter in the matching
  engine callback. Never call engine functions from the socket or render thread.
- Keep opening local and immediate; the authority distributes the resulting state.
- Initially support opening only. Closing, scripted doors and elevators require
  separate native verification and are not advertised as supported.
- Subscribe within 200 m; keep a subscription until 250 m. Remember accepted
  openings for the session so approach, reconnect and streaming can recover them.
- `sync_doors=1` is saved under `[player]`. Off suppresses both publication and
  remote application, including for a hosting player; the host still serves others.
  Off never closes an already open door. Re-enabling fetches nearby authority state.
- `door_key=` is saved under `[ui]`, unbound by default. Use the existing binding
  conflict checks and input capture. Ignore hotkeys while chatting, rebinding,
  unfocused or using the menu. The menu button remains usable with sync disabled.
- Manual opening selects one visible supported closed door in front of the camera,
  at most 3 m away. Resolve obstruction on the server simulation thread.
- Unknown/ambiguous doors and unsupported game builds fail without a state write.

## Identity and lifetime

A wire identity contains the canonical level-name hash and the original placement
matrix quantized to centimetres for position and 1/10000 for basis vectors. Compare
the full identity, not a hash alone. Validate that placement is static, independent
of the animated mesh and repeatable across game processes. Reject duplicates that
cannot be distinguished. Retain no game pointer in a network message or command.

The diagnostic process proved native opening and passability on two ordinary doors.
It did not prove multiplayer identity, generic lifecycle safety or all door types.
Production discovery must identify supported doors through verified engine data,
without repeated full-process scans. Callback arguments own the actor during a
state write; cached pointers alone are insufficient evidence of object lifetime.
World changes invalidate observations and manual requests. Received value-only
state may be reapplied to a newly loaded matching instance.

## Transport and authority

Use a new optional session packet type 18, with a separately versioned payload.
Existing protocol 8 poses remain byte-for-byte unchanged. Older hosts ignore the
new packet; the UI must report unsupported door sync rather than imply success.
Only admitted members can publish. Joining clients accept authority state only
from their host. Door messages bypass generic broadcast and mirror-player handling.

Interest, opening report, authority state and acknowledgment are distinct operations.
An interest generation changes on enable or world change, not on every heartbeat.
Every state has an authority revision and delivery ID; repeats acknowledge without
replaying animation. Receipt acknowledges storage, not a claim of game application.
Reports and state are retried, coalesced and bounded; periodic interest recovers
after an interruption. State retained by a client can wait for the object to load.

Authority logic is shared by the player host and dedicated relay. Limits: 8192
remembered doors, 1024 unacknowledged door states per peer, 128 outstanding local reports,
4 door packets per peer per 100 ms, 250 ms retry spacing and 10 s retry windows.
Never silently evict a remembered opening and present it as closed. An exhausted
limit rejects the new entry and is visible in diagnostics. Session reset clears
authority state. A 8192-bit delivered-record bitmap per peer lets the authority
page later doors after ACKs free pending slots. Movement sequence numbers are
unrelated to door delivery IDs. Interest has a generation and a monotonic heartbeat
revision so delayed subscription packets cannot reverse a toggle or position.

## Verification

Portable tests cover codec rejection, identity, packet loss and duplication,
late approach, hysteresis, streaming, disable/re-enable, simultaneous opening,
world isolation, timeout/wraparound and memory/send limits. Exercise the same
authority through dedicated-relay and Windows-host packet handling.

Game checks require two separate clients, both directions, visible opening and
physical passage, then a reload/rejoin and deliberate packet loss. Mirror mode
is not a substitute. Publish an honest draft if these checks need another tester;
do not describe multiplayer as verified from the earlier local native probes.

## Files

- `src/net/doors.h`, `src/net/doors.cpp`: portable identity, codec and authority/client state.
- `src/net/protocol.h`, `src/net/net.h`, `src/net/net.cpp`: packet routing and game-facing value API.
- `src/server/relay.h`, `src/server/relay.cpp`: dedicated authority and cleanup.
- `src/game/doors.h`, `src/game/doors.cpp`, `src/game/door_policy.h`: native adapter and target policy.
- `src/game/hooks.cpp`, `src/app/mod.cpp`: engine integration and world lifecycle.
- `src/core/config.h`, `src/core/config.cpp`, `src/ui/menu.cpp`, `src/ui/overlay.*`: saved controls.
- `tests/door_test.cpp`, existing network/session/server tests and build workflow: checks.

No game files, extracted banks, native decompilation or diagnostic DLLs belong in
the repository or the PR.
