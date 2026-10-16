#pragma once
#include <string>
namespace mecmp::game::player_audio {
bool install();
// Called from the game's client replay updater, where native sound entities run.
void tick();
std::string status();
// The local player cries out (a vocal from the catalogue, by wave name): heard here and, through the usual
// audio packets, on our ghost at the others. Any thread; played on the next client update. Ignored when the
// clip is unknown or the voice system is not ready yet.
void requestOwnVocal(const char* waveName);
}
