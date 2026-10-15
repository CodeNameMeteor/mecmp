#pragma once
// "Go to a player": move the local player to another player, with the game's own character teleport
// (ServerCharacterEntity_teleport, the call the game's teleport entities make) behind a short fade.
//
//   fade out -> teleport -> hold (kept in place until a ray finds the ground under the target, then
//   set down on it and re-placed if falling; no ground in 8 s: back where we started) -> fade in
//
// Threads: request/fadeAlpha/status from any thread; tick on the server game thread.

#include <cstdint>
#include <string>

namespace mecmp::game::teleport {

void requestGoTo(uint32_t peer); // ignored while one is running
// "Go to a place": the same fade and teleport, to where a summoner stood when it called (a summon
// carries the place, not the player). Remembers where we were for returnLeftMs / requestReturn.
// False (nothing happens) while one is running, when teleporting is off in this build, or for a place
// that is not finite.
bool requestToPos(const float pos[3], float yaw);
// After requestToPos moved us: how long (ms) requestReturn still takes us back to where we stood
// (0 = no longer possible: it ran out, the level changed, or the character died / respawned).
uint32_t returnLeftMs();
bool requestReturn();            // same rules as requestToPos; ignored when returnLeftMs() is 0
void forgetReturn();             // the player dismissed "Go back?": returnLeftMs() is 0 from now on
bool busy();
// 0..1, how black the screen should be now (the overlay draws it over everything).
float fadeAlpha();
// False once the teleport function did not look like this build's (go-to is then off; logged).
bool available();
void tick();          // server game thread, every tick
void levelChanged();  // abandon a teleport in progress

} // namespace mecmp::game::teleport
