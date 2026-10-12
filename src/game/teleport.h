#pragma once
// "Go to a player": move the local player to another player, with the game's own character teleport
// (ServerCharacterEntity_teleport, the call the game's teleport entities make) behind a short fade.
//
//   fade out -> teleport -> hold (city streams in; re-placed if falling through unloaded ground)
//   -> fade in
//
// Threads: request/fadeAlpha/status from any thread; tick on the server game thread.

#include <cstdint>
#include <string>

namespace mecmp::game::teleport {

void requestGoTo(uint32_t peer); // ignored while one is running
bool busy();
// 0..1, how black the screen should be now (the overlay draws it over everything).
float fadeAlpha();
// False once the teleport function did not look like this build's (go-to is then off; logged).
bool available();
void tick();          // server game thread, every tick
void levelChanged();  // abandon a teleport in progress

} // namespace mecmp::game::teleport
