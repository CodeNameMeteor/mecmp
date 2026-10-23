#pragma once
// What the mod knows about the running level: the player's and the ghost spawners (captured as
// the game creates them), the local player character, and a reference bone setup for ghosts.

#include <cstdint>
#include <string>
#include <vector>

#include "game/engine.h"

namespace mecmp::game::level {

struct BoneSetup {
    uint32_t count = 0;
    uint32_t indices[kMaxBones] = {};
    uint8_t flags[kMaxBones] = {};
};

// Hook callbacks (game threads).
void onSpawnerCreated(void* creationInfo, void* entity);
void onClientSpawnerCreated(void* entity);  // a ClientCharacterSpawnEntity
// Live client character spawners seen this level (client thread).
std::vector<void*> clientSpawners();
void onForeignClientReplayComp(void* comp); // a client replay component that is not one of our ghosts'
void forgetClientReplayComp(void* comp);    // ...that turned out to be one of ours after all
// Bones the replay recorder and player record: the data is patched when its owner is created, before it is built.
void patchRecordingData(uintptr_t compData);
// The player data is shared with the game's own ghosts: patched for our ghosts' clients (`ours`), vanilla for the others.
void patchPlayerData(uintptr_t compData, bool ours);
// The local recorder's setup. False if it lacks the bones we ask for (the mod loaded after the level), then ghost
// clients stay vanilla as well.
bool noteRecorderSetup(const BoneSetup& setup);

// True when everything ghosts need is known (player + ghost spawner, a ghost client for the bones).
bool complete();
// Finds missing spawners / ghost clients in memory (when the mod was loaded after the level).
// Takes a few seconds; call from the mod's own thread.
void scanForMissing();

// The level changed (or ended): forget everything captured from it.
void reset();

uintptr_t serverWorld();
// The running level's asset name ("Levels/SP/SP_MainCity/..."); empty between levels or if unreadable.
std::string levelName();
// The main menu's level ("Levels/SP/FrontEnd/..."): no ghosts there, and its menus pause as usual. Any
// thread; cached per world.
bool inFrontEnd();
void* playerSpawner();
void* ghostSpawner();                 // the level's city-ghost spawner (always loaded in free roam)
// MECMP's own ghost spawner: a spawner entity made from the city-ghost spawner's data in its bus, so the game's
// "Your Echo" character stays the city spawner's last and the city spawner never takes a ghost back. Made on first
// use and again when the city-ghost spawner changes. Null when it is off (mecmp.ini [ghost] own_spawner=0), the
// build check of the creation functions failed, it could not be made (retried after 30 s), or it gave no
// character (ownSpawnerFailed): then ghosts come from ghostSpawner() as before. Server game thread.
void* ownGhostSpawner();
// The own spawner if it was made (and is still a spawner), without making one.
void* madeOwnGhostSpawner();
// A spawn from the own spawner gave no character: it is not used again this level.
void ownSpawnerFailed(void* spawner);
// Picks the city-ghost spawner among all that were seen (own data object, holds a character that is not
// one of ours, `ours` = our ghosts' characters). Game thread; cheap (at most every 0.5 s), so call it
// before every spawn: a hook that sees a newer spawner, or a scan that replaces a dead one, can change
// the choice at any time.
void chooseGhostSpawner(const void* const* ours, size_t oursCount);
// Whether a spawn from `spawner` gave a character. After a few failures in a row the spawner is not
// chosen again until the level changes (some spawners refuse, e.g. one that already holds its maximum).
void spawnResult(void* spawner, bool gotCharacter);
void* playerCharacter();              // local player's ServerCharacterEntity (game thread)
bool spawnerAlive(void* spawner);
bool characterAlive(void* character, void* spawner); // still in its spawner's spawned list
bool characterExists(void* character, uintptr_t expectedData); // a live PamServerCharacterEntity with this data
// Bone setup the game's ghost clients use (copied from any live ghost client component).
bool referenceSetup(BoneSetup& out);
bool readSetup(uintptr_t comp, BoneSetup& out); // the playback setup (CRP_Setup)
// A bone setup object (count +0x00, indices [+0x10], flags [+0x78]); false unless it looks built.
bool readSetupAt(uintptr_t setup, BoneSetup& out);
bool readOwnSetup(uintptr_t comp, BoneSetup& out); // a client replay component's own setup (CRP_OwnSetup)

} // namespace mecmp::game::level
