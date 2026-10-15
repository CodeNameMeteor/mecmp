#pragma once
#include <string>
namespace mecmp::game::player_audio {
bool install();
// Called from the game's client replay updater, where native sound entities run.
void tick();
std::string status();
}
