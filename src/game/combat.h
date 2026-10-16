#pragma once
// Strikes between players (net/protocol.h DAMAGE). Ghosts have no collision, so a strike of the local player
// is turned into a hit by position (game/strike_math.h) and sent to the player it touched; the other side
// checks it and takes one hit point off the player's health, if the player allowed it ([combat] hits_taken).

#include <cstdint>
#include <string>

namespace mecmp::game::combat {

// The local player swung: `source` is a net::kPlayerSounds / vocal index (net/player_audio.h) captured from
// the player's own combat sound graphs, `event` the graph's event hash (0 for a vocal). Any thread; cheap.
void onLocalCombatEvent(uint8_t source, uint32_t event);

// Server game thread, every tick: decide queued strikes, handle hits sent to us, watch our health.
void tick();
void levelChanged();

// A line for the notices ("Foie hit you"), or empty. Render thread.
std::string takeNotice();

} // namespace mecmp::game::combat
