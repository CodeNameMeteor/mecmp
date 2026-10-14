#pragma once
// The marker the player places on the game's map (the tracker that Runner's Vision guides you to).
//
// How the game reports it (Ghidra): placing it sends NetworkPamMapTrackerPlacedMessage (category
// "Network" 0xc67d277b, type 0x19500757; class 0x90 bytes, ctor 0x144753330): +0x48 a bool, +0x50 a
// LinearTransform whose translation (+0x80) is the marker's world position. The map's beacon selector
// (PamMapBeaconPositionSelectorEntityData: FromPosition, LookAtPosition, IsUnderground) picks that spot
// on the city's geometry. The mod listens for the message with a listener object of its own on the
// client and server message managers (MessageManager_registerListener; no game code is changed) and
// keeps the newest position, so "go to the map marker" can teleport there.
//
// A marker placed before the mod was loaded (or kept from a save) is not known until it is placed again.

namespace mecmp::game::map_marker {

// The newest marker's position. False: none placed since the mod loaded (or in this level).
bool position(float out[3]);
void clientTick(); // client game thread: registers the listener once that context exists
void serverTick(); // server game thread: the same
void levelChanged();

} // namespace mecmp::game::map_marker
