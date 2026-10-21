#pragma once
#include <string>
namespace mecmp::game::player_audio {
// Player sound sync is disabled until it is fixed (CMake option MECMP_PLAYER_AUDIO): when false nothing below is
// installed or ticked, received sounds are dropped and the menu hides the setting. The code is kept.
#ifdef MECMP_PLAYER_AUDIO
constexpr bool kEnabled = true;
#else
constexpr bool kEnabled = false;
#endif
bool install();
// Called from the game's client replay updater, where native sound entities run.
void tick();
std::string status();
}
