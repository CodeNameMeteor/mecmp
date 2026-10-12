#pragma once
// Positional voice through Mumble's "Link" plugin: each frame the local player's position and view
// go into the shared memory block "MumbleLink", which a running Mumble reads. Players whose context
// (the session: everyone's is the host's session id) matches hear each other from where their ghosts
// are. Nothing happens without Mumble; mecmp.ini [voice] mumble_link=0 (or the menu) turns it off.
//
// Mumble's coordinates are left-handed metres (x right, y up, z forward); Frostbite's are right-handed,
// so z is negated. MEC is first person: the camera is both the avatar and the listener.

namespace mecmp::game::mumble {

void update(); // render thread, every frame

} // namespace mecmp::game::mumble
