#pragma once
// World markers ("pings"): where a new marker goes, and the hand-over from the input threads to the
// server game thread.
//
// request() only notes what to place (keyboard on the window thread, gamepad on the game's input
// thread). tick() runs on the server game thread, picks the point there and hands it to the network,
// so a later version may ask the engine for the point (see pickPoint).
//
// What the marker says comes from a marker wheel slot (config: wheels): its icon and title are the
// player's own.

namespace mecmp::game::ping {

// Any thread: place a marker at the next server tick (or once the short pause after the marker before
// it is over; the request is kept, and a newer one replaces it).
void request(int wheel, int slot); // that slot of that wheel
void request();                    // what a short press places (config: ping_tap)
void requestAlt();                 // a short press of the other-way key: the same marker, at the other kind of point
void requestMove(int index);       // move my marker in that cell (net::PingInfo::index) to where a new one would go
void tick();                       // server game thread, every tick

// The point a new marker is placed at: what the camera looks at (a ray cast into the world,
// game/raycast.h), or the local player's own position (their feet) when that is switched off
// (config: ping_aim), nothing is in reach, or the ray cast is not available in this build.
// Server game thread only.
// `otherWay`: the other kind of point than the setting says (my feet instead of the aimed one, and the
// aimed one instead of my feet).
bool pickPoint(float out[3], bool otherWay = false);

} // namespace mecmp::game::ping
