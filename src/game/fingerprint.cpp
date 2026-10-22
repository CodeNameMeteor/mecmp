#include "game/fingerprint.h"

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"

namespace mecmp::game::fingerprint {
namespace {

// A vtable slot and the function it holds in this build (taken from the PC dump in Ghidra).
// Comparing addresses (after rebasing) is immune to relocation.
struct Slot { const char* what; uintptr_t vtable; size_t offset; uintptr_t fn; };
constexpr uintptr_t Fn_ApplyCustomize = 0x14336cf00;
constexpr uintptr_t Fn_GetComponent = 0x1431c6eb0;
constexpr uintptr_t Fn_ClientSetVisible = 0x143229480;
constexpr uintptr_t Fn_SpawnerSpawn = 0x143390830;
const Slot kSlots[] = {
    {"PamServerCharacterEntity::applyCustomization", Vtbl_PamServerCharacterEntity, Vtbl_ApplyCustomize, Fn_ApplyCustomize},
    {"PamServerCharacterEntity::getComponent", Vtbl_PamServerCharacterEntity, Vtbl_GetComponent, Fn_GetComponent},
    {"PamClientCharacterEntity::setVisible", Vtbl_PamClientCharacterEntity, Vtbl_ClientSetVisible, Fn_ClientSetVisible},
    {"PamClientCharacterEntity::getComponent", Vtbl_PamClientCharacterEntity, Vtbl_GetComponent, Fn_GetComponent},
    {"PamServerCharacterSpawnEntity::spawn", Vtbl_PamServerCharacterSpawnEntity, Vtbl_SpawnerSpawn, Fn_SpawnerSpawn},
    {"ServerCharacterSpawnEntity::spawn", Vtbl_ServerCharacterSpawnEntity, Vtbl_SpawnerSpawn, Fn_SpawnerSpawn},
    {"PamServerCharacterSpawnEntity::unspawn", Vtbl_PamServerCharacterSpawnEntity, Vtbl_SpawnerUnspawn, Fn_SpawnerUnspawn},
    {"ServerCharacterSpawnEntity::unspawn", Vtbl_ServerCharacterSpawnEntity, Vtbl_SpawnerUnspawn, Fn_SpawnerUnspawn},
};

// The first instructions of functions the mod calls directly. Only position-independent bytes
// (no absolute addresses; RIP-relative displacements do not change when the image moves).
struct Code { const char* what; uintptr_t fn; size_t skip; unsigned char bytes[16]; size_t n; };
const Code kCode[] = {
    // mov rax,[rcx+18h]; test rax,rax; jz +18h
    {"NetState::setDirty", Fn_NetState_setDirty, 0, {0x48, 0x8B, 0x41, 0x18, 0x48, 0x85, 0xC0, 0x74, 0x18}, 9},
    // sub rsp,38h; mov eax,[rcx+18h]; movzx r9d,r8b
    {"SpatialEntity::setTransform", Fn_SpatialEntity_setTransform, 0,
     {0x48, 0x83, 0xEC, 0x38, 0x8B, 0x41, 0x18, 0x45, 0x0F, 0xB6, 0xC8}, 11},
    // sub rsp,28h; mov rax,[rip-156394Bh] (the client game context)
    {"localClientCharacter", Fn_LocalClientCharacter, 0, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0xB5, 0xC6, 0xA9, 0xFE}, 11},
    // mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,20h; cmp qword [rcx],0
    {"Arena::allocFromMember", Fn_Arena_allocFromMember, 0,
     {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20}, 15},
    // mov [rip-124CF96h],rax: where the game stores XInputGetState (the gamepad hook swaps it)
    {"XInput pointer store", Code_StoreXInputGetState, 0, {0x48, 0x89, 0x05, 0x6A, 0x30, 0xDB, 0xFE}, 7},
    // mov rcx,[rip-0D8C6B2h]: clientWorldFrameTick loading the camera view managers (name tags read them)
    {"camera managers load", Code_LoadCameraViewManagers, 0, {0x48, 0x8B, 0x0D, 0x4E, 0x39, 0x27, 0xFF}, 7},
    // (after mov rax,rcx) mov rcx,[rcx]; test rcx,rcx; jz +19h
    {"rbtree increment", Fn_RbTreeIncrement, 3, {0x48, 0x8B, 0x09, 0x48, 0x85, 0xC9, 0x74, 0x19}, 8},
};

// The functions the own ghost spawner is made with (level::ownGhostSpawner). Not part of check(): a mismatch
// only turns that feature off, ghosts then come from the city-ghost spawner as before.
const Code kOwnSpawnerCode[] = {
    // mov rbx,r9; mov r9d,90C3A20Fh (the creation info's base ctor argument)
    {"ServerEntityCreationInfo ctor", Fn_ServerEntityCreationInfo_ctor, 0x19, {0x49, 0x8B, 0xD9, 0x41, 0xB9, 0x0F, 0xA2, 0xC3, 0x90}, 9},
    // mov rax,rsp; push rdi; sub rsp,70h; mov qword [rax-48h],-2
    {"entityCreator spawnIntoBus", Fn_EntityCreator_spawnIntoBus, 0,
     {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x40, 0xB8, 0xFE, 0xFF, 0xFF, 0xFF}, 16},
    // (after lea rax,[vtable]) mov [rcx+8],rdx; mov [rcx+10h],r8; mov [rcx],rax
    {"ServerEntityInitInfo ctor", Fn_ServerEntityInitInfo_ctor, 7, {0x48, 0x89, 0x51, 0x08, 0x4C, 0x89, 0x41, 0x10, 0x48, 0x89, 0x01}, 11},
    // mov r8d,[rcx+18h]; mov eax,r8d; shr eax,3; test al,1; jnz +12h
    {"Entity init", Fn_Entity_init, 0, {0x44, 0x8B, 0x41, 0x18, 0x41, 0x8B, 0xC0, 0xC1, 0xE8, 0x03, 0xA8, 0x01, 0x75, 0x12}, 14},
};

std::string hex(const unsigned char* b, size_t n) {
    std::string s;
    char t[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(t, sizeof(t), "%02X ", b[i]);
        s += t;
    }
    return s;
}
} // namespace

bool check() {
    int bad = 0;
    for (const Slot& s : kSlots) {
        uintptr_t got = 0;
        if (!mem::read(rva(s.vtable) + s.offset, got) || got != rva(s.fn)) {
            MLOG("build check: %s is %p, expected %p", s.what, reinterpret_cast<void*>(got), reinterpret_cast<void*>(rva(s.fn)));
            ++bad;
        }
    }
    for (const Code& c : kCode) {
        unsigned char got[16] = {};
        if (!mem::read(rva(c.fn) + c.skip, got, c.n) || memcmp(got, c.bytes, c.n) != 0) {
            MLOG("build check: %s starts with %s, expected %s", c.what, hex(got, c.n).c_str(), hex(c.bytes, c.n).c_str());
            ++bad;
        }
    }
    if (!bad) MLOG("build check: %llu functions and vtable slots match",
                   static_cast<unsigned long long>(sizeof(kSlots) / sizeof(kSlots[0]) + sizeof(kCode) / sizeof(kCode[0])));
    return bad == 0;
}

bool checkOwnSpawner() {
    int bad = 0;
    for (const Code& c : kOwnSpawnerCode) {
        unsigned char got[16] = {};
        if (!mem::read(rva(c.fn) + c.skip, got, c.n) || memcmp(got, c.bytes, c.n) != 0) {
            MLOG("build check: %s starts with %s, expected %s (own ghost spawner off)", c.what, hex(got, c.n).c_str(),
                 hex(c.bytes, c.n).c_str());
            ++bad;
        }
    }
    return bad == 0;
}

} // namespace mecmp::game::fingerprint
