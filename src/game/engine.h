#pragma once
// Mirror's Edge Catalyst (PC, EA app build) engine addresses, layouts and small helpers.
//
// Addresses are image VAs at the PC image base 0x140000000; mem::rva() converts them to the
// running process. Everything here is read or called; the mod never patches game code. The only
// writes are vtable/creator slot swaps (hooks.cpp) and data fields of entities we own.

#include <cstddef>
#include <cmath>
#include <cstdint>

#include "core/mem.h"

namespace mecmp::game {

using mem::rva;

// --- Globals -------------------------------------------------------------------------------------
// Game menus pause the simulation through PamUISystem's per-frame update (vtable slot below,
// PamUISystem::vf40 0x143b16c50), whose only call to UIPauseController_applyToClient (0x143b16ae0)
// passes the controller at UISystem+0x8c8; its pause requests are the ints at +0x08 / +0x0c.
constexpr uintptr_t Slot_PamUISystemUpdate = 0x141cb64f0;
constexpr uintptr_t Fn_PamUISystemUpdate = 0x143b16c50;
// Friend markers on the map (game/players_map.h): the PresenceInfo manager pool's update slot, the
// manager's vtable and its assignSlot(manager, OnlineId*, Vec4* pos, priority) -> slot*.
constexpr uintptr_t Slot_PresenceInfoManagerPoolUpdate = 0x141c74bc8;
constexpr uintptr_t Fn_PresenceInfoManagerPoolUpdate = 0x143a4bb30;
constexpr uintptr_t Vtbl_PamClientPresenceInfoManagerEntity = 0x141c74be8;
constexpr uintptr_t Fn_PresenceInfoManager_assignSlot = 0x143a405e0;
// PamServerNamedChallengeEntity (dashes and other named challenges; game/runs.h): creator slot -> thunk.
constexpr uintptr_t Slot_ServerNamedChallengeCreator = 0x141c7ee00;
constexpr uintptr_t Fn_ServerNamedChallengeCreator = 0x143a834b0;
// PamServerChallengeMarkerEntity (player-made time trials): creator slot -> thunk.
constexpr uintptr_t Slot_ServerChallengeMarkerCreator = 0x141c7e9f0;
constexpr uintptr_t Fn_ServerChallengeMarkerCreator = 0x143a83460;
constexpr uintptr_t Slot_PresenceInfoManagerCreator = 0x141c74cb8; // creator vtable slot -> the create thunk
constexpr uintptr_t Fn_PresenceInfoManagerCreator = 0x143a41850;
constexpr size_t UISystem_PauseController = 0x8c8;
constexpr uintptr_t kTypeInfoListHead = 0x14215d028;
constexpr uintptr_t kServerGameWorld = 0x1425469e0; // ServerGameWorld* (null between levels)
// world +0x20 = its ServerLevel (base ctor 0x143114e40); level +0x108 = LevelData (an asset: name at
// +0x10), e.g. "Levels/SP/FrontEnd/FrontEnd" for the main menu (levelLoad_setupWorldObjects 0x14333f2e0).
constexpr size_t World_Level = 0x20;
constexpr size_t Level_Data = 0x108;
// Camera: ClientCameraViewManagers list (ClientCameraViewManagers_postSimUpdateAll, ..._postSimUpdate)
constexpr uintptr_t kClientCameraViewManagers = 0x142415e50;
constexpr uintptr_t Code_LoadCameraViewManagers = 0x1431a24fb; // mov rcx, [kClientCameraViewManagers]
constexpr size_t CamMgrs_List = 0x08;         // vector<ClientCameraViewManager*> (one per local player)
constexpr size_t CamMgr_Transform = 0x90;     // LinearTransform: right, up, forward, position
constexpr size_t CamMgr_FovYDeg = 0x80;       // final vertical field of view in degrees (with the FOV setting)
constexpr size_t CamMgr_ActiveCamera = 0x208; // active camera object
constexpr size_t Camera_BaseFovYDeg = 0xac;   // the camera's own vertical FOV before the player's setting (55)
constexpr uintptr_t kXInputGetStatePtr = 0x14256d7e0; // the game's XInputGetState (set once by 0x1437ba6c0)
constexpr uintptr_t Code_StoreXInputGetState = 0x1437ba76f; // mov [kXInputGetStatePtr], rax

// --- TypeInfo (runtime ClassInfo) ----------------------------------------------------------------
constexpr uintptr_t TI_ServerCharacterEntity = 0x1427fe1e0;
constexpr uintptr_t TI_ObjectBlueprint = 0x1427e7eb0;
constexpr uintptr_t TI_PamReplayPlayerComponentData = 0x14286b890;
constexpr uintptr_t TI_PamServerReplayPlayerComponent = 0x142858f00;
constexpr uintptr_t TI_ServerCharacterCustomizationComponent = 0x1427fe0f0;
constexpr uintptr_t TI_PamClientReplayRecordingComponent = 0x1428567b0;

// --- Hook points (creator vtable slots and the functions they normally hold) ---------------------
constexpr uintptr_t Slot_ServerCharacterSpawnFactory = 0x141b4f720;
constexpr uintptr_t Fn_ServerCharacterSpawnFactory = 0x1433820c0;
constexpr uintptr_t Slot_PamServerCharacterSpawnFactory = 0x141c4b5c0;
constexpr uintptr_t Fn_PamServerCharacterSpawnFactory = 0x143957610;
constexpr uintptr_t Slot_ClientReplayPlayerFactory = 0x141c49158;
constexpr uintptr_t Fn_ClientReplayPlayerFactory = 0x143949050;
constexpr uintptr_t Slot_ClientReplayRecordingFactory = 0x141c491c8; // PamClientReplayRecordingComponent creator (vtable 0x141c48cd0)
constexpr uintptr_t Fn_ClientReplayRecordingFactory = 0x143949120;
// The updater objects that run every client replay component's update (created with the first
// component; two of them, one per update pass): pointers at these globals, their vtable, and its
// slot 2, PamClientReplayPlayerComponent_updateAll (calls ..._update, 0x1439501a0, per component).
constexpr uintptr_t Vtbl_ClientReplayUpdater = 0x141c49190;
constexpr size_t Vtbl_ClientReplayUpdateAll = 0x10;
constexpr uintptr_t Fn_ClientReplayUpdateAll = 0x14394da40;
// PamCustomizeCharacterData's vtable: a real preset of this class is copied whole (0x70 bytes: its
// material and animation fields too) before our pieces go in.
constexpr uintptr_t Vtbl_PamCustomizeCharacterData = 0x141c883e0;
constexpr size_t PamCustomizeCharacterData_Size = 0x70;
// PamClientCharacterEntity keeps its own copy of the data's PamUseRunnersGhostShader (read at
// creation): set per ghost, so the shared character data is never changed.
constexpr size_t ClientChar_GhostShader = 0x334;
// VisualUnlockAsset (a look piece; type size 0x100): after the DataContainer header (vtable, reference
// data, name) come its own fields; RenderIn1p decides the one view it is drawn in (1 = first person).
constexpr size_t VisualUnlockAsset_Size = 0x100;
constexpr size_t DataContainer_Header = 0x18;
constexpr size_t VisualUnlock_RenderIn1p = 0xf8;
constexpr uintptr_t Slot_ClientCharacterFactory = 0x141c3cd80;
// Creator slot of PamServerCustomizeProjectionCharacterEntity (time-trial marker look picker): holds the
// create thunk 0x143a83490, which calls the factory 0x143a7b5a0.
constexpr uintptr_t Slot_CustomizeProjectionFactory = 0x141c7eb40;
constexpr uintptr_t Fn_CustomizeProjectionCreate = 0x143a83490;
constexpr uintptr_t Fn_ClientCharacterFactory = 0x143910d20;
constexpr uintptr_t Slot_ServerWorldPostUpdate = 0x141b42d28; // ServerGameWorld vtable +0x28
constexpr uintptr_t Fn_ServerWorldPostUpdate = 0x143341ee0;

// --- Vtables used to recognise live objects ------------------------------------------------------
constexpr uintptr_t Vtbl_PamServerCharacterSpawnEntity = 0x141c4b388;
constexpr uintptr_t Vtbl_ServerCharacterSpawnEntity = 0x141b4e0c8;
constexpr uintptr_t Vtbl_PamClientReplayPlayerComponent = 0x141c48a28;
constexpr uintptr_t Vtbl_PamClientCharacterEntity = 0x141c3c760;
constexpr uintptr_t Vtbl_PamServerCharacterEntity = 0x141c3fcb0;
constexpr uintptr_t Vtbl_PamServerCustomizeProjectionCharacterEntity = 0x141c7ea80; // time-trial marker look picker

// --- Functions ------------------------------------------------------------------------------------
constexpr uintptr_t Fn_SpatialEntity_setTransform = 0x1432eb1f0; // (entity, const LinearTransform*, u8)
// The game's character teleport, as its teleport entities use it (ServerTeleportEntity_teleportPlayer
// 0x143391430): (ServerCharacterEntity*, const LinearTransform* 16-byte aligned).
constexpr uintptr_t Fn_ServerCharacterEntity_teleport = 0x143378830;
// The ray caster behind the game's ray cast entities (ServerRaycastEntity; called per entity from
// 0x143a204b0): (const Query*, world [unused], physics, Result*), layouts in raycast.cpp. A plain ray
// goes on to 0x1434e27f0 (physics, input, raw hit) with collision filter 9 and nothing ignored.
constexpr uintptr_t Fn_RaycastQuery = 0x143a24890;
constexpr uintptr_t Fn_NetState_setDirty = 0x14347e420;          // (netState, bits)
constexpr uintptr_t Fn_Arena_allocFromMember = 0x142a647d0;      // (&arenaMember, size, align)
constexpr uintptr_t Fn_LocalClientCharacter = 0x1439655f0;       // () -> local PamClientCharacterEntity
constexpr uintptr_t Fn_RbTreeIncrement = 0x142a34820;            // EASTL rbtree successor
constexpr size_t Vtbl_SpawnerSpawn = 0xd0;     // ServerCharacterSpawnEntity::spawn(spawner, SpawnInfo*)
// ServerCharacterSpawnEntity::unspawn(spawner, character, bool destroy, 0): what the game uses to take a replay
// ghost away (PamServerReplayEntity on StopReplay, 0x1439741d0). Releases a player made for the character, takes it
// out of the spawner's list (Spawner_Spawned) and destroys it (or keeps it for the spawner's next spawn). Does
// nothing when the character is not in that list.
constexpr size_t Vtbl_SpawnerUnspawn = 0x108;
constexpr uintptr_t Fn_SpawnerUnspawn = 0x14338e4c0; // both spawner classes
// Creating a server entity from its data, as the game does when a sublevel spawns its entities
// (levelSublevel_spawnEntityGroup): creation info, create into a bus, init. MECMP makes its own character
// spawner this way (level::ownGhostSpawner). Checked separately (fingerprint::checkOwnSpawner): a mismatch only
// turns the own spawner off.
constexpr uintptr_t Fn_ServerEntityCreationInfo_ctor = 0x14335cab0; // (ci[0x200], data, bus, const LinearTransform*)
constexpr uintptr_t Fn_EntityCreator_spawnIntoBus = 0x14335e240;    // (ci, parentBus = 0) -> entity
constexpr uintptr_t Fn_ServerEntityInitInfo_ctor = 0x14335cb50;     // (info[0x30], 0, 0)
constexpr uintptr_t Fn_Entity_init = 0x1431661c0;                   // (entity, initInfo)
constexpr size_t Vtbl_GetComponent = 0x108;    // character->getComponent(TypeInfo*)
constexpr size_t Vtbl_ApplyCustomize = 0x340;  // PamServerCharacterEntity: apply CustomizeCharacterData
constexpr size_t Vtbl_ClientSetVisible = 0x3d8; // PamClientCharacterEntity: setVisible(bool)

// --- Layouts --------------------------------------------------------------------------------------
constexpr size_t Asset_Name = 0x10;
constexpr size_t ReferenceObjectData_Blueprint = 0x18;
constexpr size_t ObjectBlueprint_Object = 0x40;   // EntityData* (type dump; validated before use)
constexpr size_t Component_Collection = 0x08;     // fb::Component: owner's component collection; [0] = owner entity
constexpr size_t CI_Bus = 0x10;           // EntityCreationInfo
constexpr size_t CI_Data = 0xd0;          // EntityCreationInfo: the entity's data
constexpr size_t Spawner_Bus = 0x28;      // the entity bus it lives in (characters it spawns get a child bus of it)
constexpr size_t Spawner_Data = 0x30;     // CharacterSpawnReferenceObjectData
constexpr size_t Spawner_BusCounter = 0xf4; // next child bus id when SpawnInfo::busId is -1 (& 0x7f)
constexpr size_t Spawner_Spawned = 0x148; // vector<character*> begin/end
constexpr size_t Spawner_Enabled = 0x2a4;
constexpr size_t SpawnerData_Template = 0x198;   // -> CharacterSpawnTemplateData
constexpr size_t Template_Customization = 0x18;  // -> CustomizeCharacterData
constexpr size_t Character_Data = 0x30;          // PamCharacterEntityData
constexpr size_t Character_Player = 0x250;
// A character's own entity bus (server and client) and the bus's id within its parent: the id given at creation
// is stored as (id + 0x1ffff80) & 0x1ffffff (entityObject_initWithParentTransform 0x1431411d0).
constexpr size_t Character_Bus = 0x28;
constexpr size_t Bus_Id = 0x58;
constexpr uint32_t Bus_IdMask = 0x1ffffff;
constexpr size_t CharData_GhostShader = 0xe8;    // PamCharacterEntityData::PamUseRunnersGhostShader
constexpr size_t Entity_TransformSpace = 0x38;
// ServerGameWorld -> its physics, as the ray cast entities' update reads it (0x143a1fd7a and the
// getter 0x1434ee940), and what a ray cast reads from the physics object (0x14351a1a0).
constexpr size_t World_PhysicsManager = 0x50, PhysManager_Holder = 0xf0, PhysHolder_Physics = 0x18;
constexpr size_t Phys_HavokWorld = 0x10, HavokWorld_FilterOwner = 0x150, FilterOwner_Filter = 0x4d0;

// PamServerReplayPlayerComponent: playback object at +0x30 (state at +0x38, 1 = playing).
constexpr size_t SRP_PlaybackState = 0x38, SRP_Setup = 0xb0, SRP_Pose = 0x178, SRP_PoseEnd = 0x180,
                 SRP_PoseCap = 0x188, SRP_PoseArena = 0x190, SRP_NetState = 0x1a0;
// PamClientReplayPlayerComponent: own playback at +0x38 (state +0x40, forced +0xb0, setup +0xb8),
// received pose +0x180, skipRoot +0x2e6. Own bone setup +0x200 (count, indices [+0x210]), built at init from the
// data's RecordingBones; the received pose only counts when the server's bone count equals it (+0x2e4).
constexpr size_t CRP_PlaybackState = 0x40, CRP_PlaybackForced = 0xb0, CRP_Setup = 0xb8,
                 CRP_ReceivedPose = 0x180, CRP_OwnSetup = 0x200, CRP_CountOk = 0x2e4, CRP_SkipRoot = 0x2e6;
// Local replay recorder (PamClientReplayRecordingComponent +0x48): keyframe ring.
constexpr size_t Rec_Stride = 0x68, Rec_Capacity = 0x70, Rec_Base = 0x78, Rec_Head = 0x88;
constexpr size_t Rec_Setup = 0xd8; // the recorder's bone setup (same layout as the client's)

constexpr const char* kGhostBlueprint = "Characters/Projection/ProjectionCharacter";
constexpr const char* kPlayerBlueprint = "Characters/FaithCharacter";
constexpr uint32_t kMaxBones = 96;

// --- Types ----------------------------------------------------------------------------------------
struct alignas(16) LinearTransform {
    float right[4], up[4], forward[4], trans[4];
};
static_assert(sizeof(LinearTransform) == 0x40, "LinearTransform");

// SpawnInfo for ServerCharacterSpawnEntity::spawn.
struct alignas(16) SpawnInfo {
    LinearTransform transform;
    void* player = nullptr;
    void* existing = nullptr;
    void* unk50 = nullptr;
    int32_t unk58 = -1;
    int32_t busId = -1;
    uint8_t unk60 = 1;
    uint8_t useTransform = 0;
    uint8_t pad[0x0e] = {};
};
static_assert(offsetof(SpawnInfo, player) == 0x40 && offsetof(SpawnInfo, busId) == 0x5c &&
              offsetof(SpawnInfo, useTransform) == 0x61, "SpawnInfo layout");

// --- Helpers --------------------------------------------------------------------------------------
inline bool inImage(uintptr_t a) {
    const uintptr_t base = mem::moduleBase();
    return a >= base && a < base + 0x727b000;
}

// The function in vtable slot `slotOffset` of `obj`, if the object is readable and both its vtable
// and that function lie in the game image (0 otherwise). Guards every virtual call on an object
// the mod did not create itself (B-11).
inline uintptr_t gameVirtual(const void* obj, size_t slotOffset) {
    uintptr_t vt = 0, fn = 0;
    if (!obj || !mem::read(reinterpret_cast<uintptr_t>(obj), vt) || !inImage(vt)) return 0;
    if (!mem::readFast(vt + slotOffset, fn) || !inImage(fn)) return 0; // vtables are image data
    return fn;
}

// `classInfo` is a runtime address (see isKindOf below for an image VA).
inline bool isKindOfRuntime(void* obj, uintptr_t classInfo) {
    using GetTypeFn = const uint8_t* (*)(void*);
    const auto getType = reinterpret_cast<GetTypeFn>(gameVirtual(obj, 0));
    if (!getType) return false;
    const uint8_t* ti = getType(obj);
    uint16_t id = 0, range[2] = {}; // the class's first and last type id
    if (!ti || !mem::readFast(reinterpret_cast<uintptr_t>(ti) + 0x28, id) || !mem::readFast(classInfo + 0x28, range, sizeof(range)))
        return false;
    const uint16_t first = range[0], last = range[1];
    return static_cast<uint32_t>(id - first) <= static_cast<uint32_t>(last - first);
}

inline bool isKindOf(void* obj, uintptr_t classInfoVa) { return isKindOfRuntime(obj, rva(classInfoVa)); }

inline void* getComponent(void* character, uintptr_t tiVa) {
    using Fn = void* (*)(void*, const void*);
    const auto fn = reinterpret_cast<Fn>(gameVirtual(character, Vtbl_GetComponent));
    return fn ? fn(character, reinterpret_cast<const void*>(rva(tiVa))) : nullptr;
}

// Live world transform of a spatial entity (read fault-tolerantly). Game thread only.
inline bool readEntityTransform(const void* entity, LinearTransform& out) {
    const uintptr_t ts = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(entity) + Entity_TransformSpace);
    uint8_t sel[2] = {};
    if (!ts || !mem::read(ts + 9, sel, 2)) return false;
    const size_t idx = static_cast<size_t>(sel[0]) + 2u * sel[1];
    return mem::read(ts + 0x10 + idx * 0x20, &out, sizeof(out));
}

inline void setTransform(void* entity, const LinearTransform& t) {
    reinterpret_cast<void (*)(void*, const LinearTransform*, uint8_t)>(rva(Fn_SpatialEntity_setTransform))(entity, &t, 0);
}

inline void setNetDirty(void* netState, uint64_t bits) {
    reinterpret_cast<void (*)(void*, uint64_t)>(rva(Fn_NetState_setDirty))(netState, bits);
}

inline void* engineAlloc(void** arenaMember, uint64_t size, uint64_t align) {
    return reinterpret_cast<void* (*)(void**, uint64_t, uint64_t)>(rva(Fn_Arena_allocFromMember))(arenaMember, size, align);
}

// Fault-tolerant small write (our own process): used for fields the client thread also touches.
inline void poke(uintptr_t addr, const void* src, size_t n) { mem::write(addr, src, n); }
template <class T> inline void poke(uintptr_t addr, T v) { poke(addr, &v, sizeof(T)); }

inline LinearTransform transformFromYaw(const float pos[3], float yaw) {
    LinearTransform x{};
    const float c = std::cos(yaw), s = std::sin(yaw);
    x.right[0] = c;  x.right[2] = -s;
    x.up[1] = 1;
    x.forward[0] = s; x.forward[2] = c;
    x.trans[0] = pos[0]; x.trans[1] = pos[1]; x.trans[2] = pos[2]; x.trans[3] = 1;
    return x;
}

} // namespace mecmp::game
