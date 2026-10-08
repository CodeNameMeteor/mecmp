#pragma once
// The local player's state for the network: position, facing and body pose.
//
// The pose comes from the player's own replay recorder (PamClientReplayRecordingComponent +0x48),
// which always records the last ~15 s for the game's "create a time trial" feature. Its keyframes
// are in exactly the format a ghost's replay player plays: 23 bones of {vec4 translation, quat}.

#include "net/net.h"

namespace mecmp::game::local_player {

// Fills position, yaw and the newest recorded pose. Game thread. False when not in a level.
bool capture(net::PlayerState& out);

} // namespace mecmp::game::local_player
