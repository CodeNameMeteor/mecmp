#include "game/native_vocals.h"
#include "game/engine.h"

namespace mecmp::game::native_vocals {
namespace {
constexpr uintptr_t kSystem = 0x1423e3150;
constexpr uintptr_t kBusLevel = 0x14314c5e0;
constexpr uintptr_t kTrigger = 0x1430477f0, kPronunciation = 0x143046760;
constexpr uintptr_t kTakeControls = 0x143046940, kSources = 0x1430467f0, kCancel = 0x143042720;
}
uint16_t realmForBus(uintptr_t bus) {
    if (!bus) return 0;
    const auto level = reinterpret_cast<uintptr_t(*)(uintptr_t)>(rva(kBusLevel))(bus);
    return level ? mem::readOr<uint16_t>(level + 0x90) : 0;
}
bool start(Playback& p, uintptr_t data, uintptr_t character, uint16_t realm) {
    p.system = mem::readOr<uintptr_t>(rva(kSystem));
    if (!p.system || !character || mem::readOr<uintptr_t>(character) != rva(Vtbl_PamClientCharacterEntity) ||
        !mem::read(data + 0x18, p.conversation.data(), p.conversation.size())) return false;
    const auto info = reinterpret_cast<uintptr_t>(p.conversation.data());
    // The catalogue accepts only the one-group, one-track Faith effort clips.
    for (size_t offset : {size_t(0), size_t(8)}) {
        const auto list = mem::readOr<uintptr_t>(info + offset);
        if (!list || mem::readOr<uint32_t>(list - 4) != 1) return false;
    }
    reinterpret_cast<void(*)(uintptr_t, uintptr_t, uint16_t, uint32_t*)>(rva(kTrigger))(p.system, info, realm, &p.handle);
    if (!p.handle) return false;
    const auto pronunciation = mem::readOr<int32_t>(data + 0x58, -1);
    reinterpret_cast<void(*)(uintptr_t, uintptr_t, int32_t)>(rva(kPronunciation))(p.system, info, pronunciation);
    const float take = 0.f;
    reinterpret_cast<void(*)(uintptr_t, uintptr_t, const float*, uint32_t, uint32_t)>(rva(kTakeControls))(
        p.system, info, &take, 0, 1);
    // Native source adapters follow the remote character's transform. Do not
    // use the local player's voice entity, which would place all grunts here.
    reinterpret_cast<void(*)(uintptr_t, uintptr_t, const uintptr_t*, uint32_t, uint32_t)>(rva(kSources))(
        p.system, info, &character, 0, 1);
    return true;
}
void cancel(const Playback& p) {
    if (p.handle && p.system == mem::readOr<uintptr_t>(rva(kSystem)))
        reinterpret_cast<void(*)(uintptr_t, uint32_t)>(rva(kCancel))(p.system, p.handle);
}
bool referenced(const Playback& p) {
    if (!p.handle || p.system != mem::readOr<uintptr_t>(rva(kSystem))) return false;
    // Cancel is deferred. A new conversation can still be in the submission
    // queue, before its handle appears in the active tree; retain its info in
    // either case and retry cancellation on the next client update.
    uintptr_t begin = 0, end = 0;
    if (!mem::read(p.system + 0x17df0, begin) || !mem::read(p.system + 0x17df8, end) ||
        end < begin || (end - begin) % 0x20 || end - begin > 4096 * 0x20) return true;
    for (auto entry = begin; entry < end; entry += 0x20) {
        uint32_t handle = 0;
        if (!mem::read(entry + 0x10, handle)) return true;
        if (handle == p.handle) return true;
    }
    uintptr_t node = 0;
    if (!mem::read(p.system + 0x15e20, node)) return true;
    for (int depth = 0; node && depth < 64; ++depth) {
        uint32_t handle = 0;
        if (!mem::read(node + 0x20, handle)) return true;
        if (handle == p.handle) return true;
        if (!mem::read(node + (p.handle < handle ? 8 : 0), node)) return true;
    }
    return node != 0; // a corrupt tree must not free an engine-referenced info
}
}
