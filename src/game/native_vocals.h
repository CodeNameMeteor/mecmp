#pragma once
#include <array>
#include <cstdint>

namespace mecmp::game::native_vocals {
// The native conversation retains this plain struct until playback ends.
// Keep its address stable; all referenced groups/tracks remain engine-owned.
struct Playback {
    alignas(8) std::array<uint8_t, 48> conversation{};
    uintptr_t system = 0;
    uint32_t handle = 0;
    uint32_t unreferencedAt = 0;
};
uint16_t realmForBus(uintptr_t bus);
bool start(Playback& playback, uintptr_t data, uintptr_t character, uint16_t realm);
void cancel(const Playback& playback);
bool referenced(const Playback& playback);
}
