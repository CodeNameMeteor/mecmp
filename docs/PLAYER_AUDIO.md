# Player sounds and pose limits

MECMP retains its UDP hosts, join codes and dedicated servers. This change does not
add Steam integration or change player names. Use the updated DLL on both players
and the updated server binary when using a dedicated server.

## Player sounds

The Players tab has **Other players' game sounds**, a distance setting, and
capture/playback counters. It is off by default until playback has been play-tested;
capture and sending still run, so a player who turns it on hears the others. Defaults in `mecmp.ini`:

```ini
[audio]
player_sounds=0
distance=80
```

This is native game sound playback: footsteps and hand contacts, cloth movement,
pipe/ladder/bar movement, landings, breathing, effort grunts, punches, kicks,
combat impacts, damage and near-miss sounds. Microphone voice chat remains the
existing Mumble option.

The allowlist contains 25 sound graphs and 15 effort conversations from the local
character's `Sound/Logic/Faith_Movement_Audio`, `Sound/Logic/Faith_Combat_Audio`,
and nested `ME_Sound/Character/Faith_Emotes` prefabs. Vocal clips include jumps,
soft/hard landings, bumps, scraping, strain, punches, damage and hurt/falling
breathing. Story dialogue, music, and wind are excluded. Existing movement source
indices retain their meaning. Both clients and dedicated servers need this build
to accept the additional source indices.

SoundEntity's event handler captures discrete movement events and the scalar
inputs at that instant, including material selectors. Active sources also refresh
changing scalar inputs and send a heartbeat. Packets contain a source index, event
hash, timestamp and up to 16 finite scalar inputs. AudioGraph asset pointers,
arbitrary names, transforms, and asset-valued parameters are never transmitted.
The native updater's active entity list discovers breathing loops that started
before the audio catalogue became ready, without a heap scan.

`VoiceOverConversationEntity` captures Trigger/Cancel edges only for the 15
allowlisted effort clips. Vocal packets contain no scalar parameters or asset
names. Receiving clients submit their own loaded conversation through the native
voice-over system and bind its track source to that remote player's client ghost.
Each playback owns stable conversation-info storage; cancellation retains that
storage until the native pending queue and active handle tree release it.

The client replay updater consumes received events on the pose playback clock.
It creates a separate native sound instance for each remote player/source, applies
only inputs declared by the locally loaded graph, and updates its native world
transform from that player's bounded pose. Sound graph instances stop after two
seconds without input. Effort conversations have a bounded lifetime based on the
loaded clip duration. Both stop after stale movement, on leaving range/session/level,
or when sounds are disabled. Queues, sources, peer counts and incoming event rates
are bounded; each receiver accepts at most 64 distinct audio events per peer per second.

The host validates audio packet structure before relaying. Receiving clients also
validate graph events and scalar input hashes against their own movement assets.
Older DLLs ignore the optional type 14 packet. An older dedicated server will not
relay it; update that server for sound.

## Bone protections

```ini
[protection]
bone_reach=1
```

The host's **Bone reach limit** slider is 0.5–2 metres. Dedicated servers use
`bone_reach=1` in `mecmp_server.ini`. Both clamp before forwarding and emit a full
sanitized pose, preserving delta decoding from the original sender independently.
Each receiver enforces its own 2 metre ceiling, also immediately before native
pose writes. Limits cannot be disabled by another player.

MEC replay poses contain translation, a homogeneous weight and rotation; they do
not send scale vectors. Every weight is forced to 1 and every quaternion is
normalized, preventing those fields from enlarging the body. Bone count, position,
finite values, pose masks and packet lengths are checked. Yaw is wrapped before
interpolation so an extreme finite value cannot stall its angle loop.

Slot 1 is AITrajectory and is pinned to the player's validated world position. Slot
2 is the moving hip/body root and retains a fixed 2 metre allowance for parkour.
Other translations are clamped relative to their parent, along their original
direction. The trajectory never receives the limb-distance clamp.

## Native build notes

PC addresses used by the audio path:

| Native operation | Address / slot |
| --- | --- |
| SoundEntity vtable / handleEvent | `0x141ad0338` / `+0x20` → `0x14301a170` |
| Sound system global / vtable | `0x1423dd580` / `0x141abd460` |
| Create sound | system virtual `+0x40` → `0x142fe2ee0` |
| Sound instance vtable | `0x141acfad0` |
| Graph event / scalar input | instance virtual `+0x00` / `+0x18` |
| Start/stop / deferred release | instance virtual `+0x40` / `+0x48` |
| Transform / velocity | instance virtual `+0x68` / `+0x78` |
| Scalar property lookup | `0x142a424b0` |
| Vocal entity vtable / handleEvent | `0x141b2e078` / `+0x20` → `0x1432bc760` |
| Voice-over system global / submit conversation | `0x1423e3150` / `0x1430477f0` |
| Pronunciation / take controls / sources | `0x143046760` / `0x143046940` / `0x1430467f0` |
| Cancel conversation | `0x143042720` |

The event, graph, property and lifetime paths were inspected in the PC image.
Read-only inspection of the running game confirmed the sound manager vtable,
Faith's movement prefab (150 objects), graph input arrays, and a 23-bone recorder
pose with unit weights/rotations and parent-local limb translations. Hook
installation checks the expected handler; incompatible builds fail closed.
Further read-only inspection confirmed Faith's nested effort conversations, the
combat prefab, the active breathing instance, and the native sound updater list.
The conversation handler, submission, source binding, cancellation and retained
conversation-info lifetime were inspected in the PC image.

Validation for this change is a Release build and source review. Multiplayer
sound playback has not been play-tested.
