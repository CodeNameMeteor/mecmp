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
// Mod thread. Reads the list from a look-picker entity seen being created (onCustomizeEntity); with
// `allowScan`, falls back to a memory scan (a second at most; for a level that was running before the
// mod loaded). True if the list is known.
bool findEchoes(bool allowScan);
// Hook (level loading thread): a PamServerCustomizeProjectionCharacterEntity was created.
void onCustomizeEntity(void* entity);
bool applyEcho(void* ghost, uint32_t hash); // game thread
uint32_t defaultEcho();
bool echoRenders(uint32_t hash); // catalogue contains this look

// Applies a model to a ghost (game thread). `wanted` = "faith", "faith:<n>" (Faith with the outfit
// changes n, net::protocol::kWear... bits: other pieces of hers that are loaded in free roam) or part
// of a model name. False for an outfit whose pieces are not known yet: try again later.
bool apply(void* ghost, const std::string& wanted, std::string* usedName = nullptr);

// The LOCAL player's own look: with `enabled`, the player character is given the echo look `echoHash`
// (0 = Faith with the outfit changes `outfit`), first-person pieces included, and it is put back after
// a respawn or when the game changes the look itself. Off, or Faith with nothing changed: the game's
// own look.
void setOwnLook(uint32_t echoHash, uint8_t outfit, bool enabled); // any thread
void tick();                                                       // server game thread, every tick
// Mod thread: lists the look pieces loaded in this level once an outfit or the own look needs them
// (reads the game's whole heap, a second or two). True if the list is known or nobody needs it.
bool findPieces();

} // namespace mecmp::game::models
