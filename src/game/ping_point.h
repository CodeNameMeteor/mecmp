#pragma once
// World markers ("pings", experimental): where a new marker goes, and the hand-over from the input
// threads to the server game thread.
//
// request() only sets a flag (keyboard on the window thread, gamepad on the game's input thread).
// tick() runs on the server game thread, picks the point there and hands it to the network, so a
// later version may ask the engine for the point (see pickPoint).

namespace mecmp::game::ping {

void request(); // any thread: place my marker at the next server tick
void tick();    // server game thread, every tick

// The point a new marker is placed at. v1: the local player's own position (their feet).
// RESEARCH (v2): the point the camera looks at, from a ray cast into the world. That needs a game
// call whose address is not known yet; when it is, only this function changes.
bool pickPoint(float out[3]);

} // namespace mecmp::game::ping
