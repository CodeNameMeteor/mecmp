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
void requestMove(int index);       // move my marker in that cell (net::PingInfo::index) to where a new one would go
void tick();                       // server game thread, every tick

// The point a new marker is placed at. v1: the local player's own position (their feet).
// RESEARCH (v2): the point the camera looks at, from a ray cast into the world. That needs a game
// call whose address is not known yet; when it is, only this function changes.
bool pickPoint(float out[3]);

} // namespace mecmp::game::ping
