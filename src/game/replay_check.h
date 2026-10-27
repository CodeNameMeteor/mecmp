#pragma once
// Safety net for time-trial ghosts recorded with more bones than the game records (earlier MECMP betas recorded
// 85, and those runs are still on the servers). Playing one T-posed the ghost and corrupted the heap,
// for players with or without MECMP. A downloaded recording like that is refused before the game loads it: the
// game then takes it as a failed download (no ghost), and the player is told why.

#include <cstdint>
#include <string>
#include <vector>

namespace mecmp::game::replay_check {

// Server game thread, before the projection spawner's download handler runs (hooks.cpp). Returns the message's
// "succeeded" flag if the recording was refused (it is cleared; give it back with restore()), null otherwise.
uint8_t* beforeReplayDownloaded(void* message);
void restore(uint8_t* okFlag);
// Notices for the player since the last call (ui/notices.cpp, render thread).
std::vector<std::string> takeNotices();

} // namespace mecmp::game::replay_check
