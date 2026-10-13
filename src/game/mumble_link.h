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

// What the menu shows. Mumble does not answer through the link, so "linked" cannot be known for sure:
// mumbleFound means a mumble.exe process runs (checked every 3 s while asked) or Mumble had the link
// memory open before us; sending means positions went out in the last second.
struct Status {
    bool enabled = false;     // mecmp.ini [voice] mumble_link
    bool mumbleFound = false;
    bool sending = false;
    bool inSession = false;   // a context is set: only players of the same session hear each other
    char context[32] = {};
};
Status status(); // render thread

} // namespace mecmp::game::mumble
