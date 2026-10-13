#pragma once
// Ghost looks ("models").
//
// A character's look is the set of VisualUnlockAssets (mesh/outfit pieces) in its
// ServerCharacterCustomizationComponent. We build a CustomizeCharacterData of our own from a list
// of pieces and apply it to a ghost with character->vtbl[0x340]; the look replicates to the client.
//   "faith"   the local player's current outfit (3rd-person pieces only)
//   others    every CustomizeCharacterData seen in a spawner template in this level (the red
//             projection ghost "RunVis", enemies, ...), matched by part of the name.
// The ghost character data's PamUseRunnersGhostShader flag is turned off for normal models (they
// are invisible with the ghost shader) and on for projection models.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mecmp::game::models {

void noteTemplate(uintptr_t spawnerData, const std::string& spawnerName); // from spawner creation
void reset();                                                               // level changed

// The game's own echo (runner ghost) looks: style x colour, identified by the game's hash.
struct Echo {
    uint32_t hash = 0;
    uintptr_t ccd = 0; // CustomizeCharacterData
    std::string name, style, colour;
};
// The catalogue; empty until findEchoes() succeeded (in a level with time-trial markers). Never changes once returned.
std::shared_ptr<const std::vector<Echo>> echoes();
bool findEchoes();          // mod thread: memory scan, a second at most; true if the list is known
bool applyEcho(void* ghost, uint32_t hash); // game thread
uint32_t defaultEcho();
bool echoRenders(uint32_t hash); // catalogue contains this look

// Applies a model to a ghost (game thread). `wanted` = "faith" or part of a model name.
bool apply(void* ghost, const std::string& wanted, std::string* usedName = nullptr);

} // namespace mecmp::game::models
