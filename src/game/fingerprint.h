#pragma once
// Game-build fingerprint (S-8): before any hook goes in, checks that the code the mod calls and the
// vtable slots it relies on are exactly what this build of the mod was made for. A different game
// build (a patch, another store's exe) is refused instead of being called into blindly.

namespace mecmp::game::fingerprint {

// Logs every mismatch. True when everything matches.
bool check();
// The entity-creation functions the own ghost spawner needs. False (logged) turns only that feature off.
bool checkOwnSpawner();
// The offsets the full-body pose is read with (game/body_pose.h). False (logged) turns only the full body off.
bool checkBodyPose();
// The download message layout the replay safety net reads (game/replay_check.h). False (logged) leaves it out.
bool checkReplayCheck();

} // namespace mecmp::game::fingerprint
