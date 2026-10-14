#pragma once
// Players on the map: the game's own friend markers (Gameplay/Social/Presence/PF_PresenceInfo), fed with
// the session's players instead of online friends.
//
// How the game does it (Ghidra labels PresenceInfoManager_*): a PamClientPresenceInfoManagerEntity in the
// level keeps a fixed array of slots (count from PamSocialPlaySettings). Every 10 s it publishes your own
// world position to your online presence, reads your friends' presence and assigns each friend to a slot
// (assignSlot: online id, position, priority); the first time a slot is used it spawns a PF_PresenceInfo
// prefab for it. Every frame it copies each slot into that prefab's PamClientPresenceInfoEntity, whose
// WorldPosition drives a PamUIMapMarkerEntity (the map icon), name and online id drive the tooltip, and
// Valid && "shown" (slot byte 0, set from a privacy lookup) make it visible.
//
// The mod: just before the manager pool's update (a vtable slot), each present player who shares their
// EA profile (online id + name, sent in HELLO; config share_profile) gets a slot of its own (assignSlot
// with a priority above friends), and its position and "shown" flag are written into the slot every
// frame. A player who leaves has its slot cleared and hidden. The marker's icon is the profile's Runner
// Tag, loaded by online id, so markers need online services (Beat Revival) and a real id.
#include <cstdint>

namespace mecmp::game::players_map {

// Called on the client game thread, before the game's PresenceInfo manager pool updates.
void beforeManagerUpdate(void* pool);

// Server game thread, now and then: what net sends as our profile (config shareProfile, online id).
void publishOwnProfile();

// "players on map: 3 of 5 slots" etc. for F9.
const char* statusWord();

} // namespace mecmp::game::players_map
