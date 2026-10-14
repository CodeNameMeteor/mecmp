# Players on the map (the game's friend markers)

Research 2026-10-14, from the PC dump (Ghidra labels `PresenceInfoManager_*`) and the game data.

## How the game showed friends
- `Gameplay/Social/Presence/PF_PresenceInfo` is one friend marker: a `PamPresenceInfoEntityData` whose
  `WorldPosition` drives a `PamUIMapMarkerEntityData.Transform`; `LocalizedDisplayName` feeds the
  `PlayerTooltip` widget; `OnlineId` feeds `PlayerOtherIcon` and the profile panel;
  `Valid && 0xcacba3e1` ("shown") drives the marker's `Visible`.
- `PamClientPresenceInfoManagerEntity` (factory `0x143a35310`, vtable `0x141c74be8`) holds a fixed array
  of slots at +0x50/+0x58 (0x50 bytes each; count at +0x48 from PamSocialPlaySettings):
  `+0 u8 shown, +4 i32 priority, +8 OnlineId {u64 id; char name[32]}, +0x30 Vec4 position,
  +0x40 prefab instance, +0x48 handle to its PamClientPresenceInfoEntity`.
- Every 10 s it publishes your own position to your online presence (`worldPosition`, rich presence),
  reads friends' presence and calls `assignSlot(mgr, OnlineId*, Vec4* pos, priority)` (`0x143a405e0`)
  for each. assignSlot reuses the slot with that id, else a free one, else one of lower priority, and
  spawns the PF_PresenceInfo prefab the first time a slot is used. Every 300 s a privacy lookup sets each
  slot's "shown" byte. Every frame `PresenceInfoManager_update` (`0x143a50b80`) copies each slot into its
  prefab (position, name, id, valid, shown).
- The managers are updated by a pool object (vtable `0x141c74bb8`); its slot 2 (`0x141c74bc8`, function
  `0x143a4bb30`) walks pool+0x28/+0x30 ({manager*, interval, counter, time} entries) on the client thread.

## What the mod does (`game/players_map.cpp`)
The pool's update slot is swapped (data only) for a wrapper that, before the game's update, gives each
present player a slot through the game's own assignSlot (online id `"MECM" << 32 | session`, the
player's name, priority 1000, above friends at 0) and writes its position and shown = 1 every frame.
A player who leaves gets shown = 0 and an empty id (the slot is free again). `[ui] players_on_map`.

Open: the slot count in practice (logged once), whether the manager exists without online services,
and what the avatar icon shows for a made-up id.
