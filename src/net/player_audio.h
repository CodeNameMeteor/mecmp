#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace mecmp::net {
// Only bounded indices, event hashes and scalar inputs travel over the network.
inline constexpr const char* kMovementSounds[] = {
    "Footsteps/Faith_Footplant", "Handsteps/Faith_Handplant", "Movement",
    "Faith_LandIntoHang", "Handsteps/PipeSlide", "Faith_HeadAgainstCeiling",
    "Footsteps/TarpaulinLanding", "Faith_LandOnPipe", "Handsteps/PipeRelease",
    "Faith_LandIntoLadder", "Faith_LandIntoPipe", "Faith_HandsAgainstWall",
    "Faith_CharacterAgainstWall", "Footsteps/Metal/MetalGantryCreaks",
    "Handsteps/MonkeyBar_Swing", "Faith_LandIntoBar", "Handsteps/LadderClimb"
};
constexpr size_t kMovementSoundCount = sizeof(kMovementSounds) / sizeof(*kMovementSounds);
constexpr size_t kMaxAudioParams = 16;
struct AudioParam { uint32_t key = 0; float value = 0; };
struct PlayerAudio {
    uint32_t id = 0, timeMs = 0, event = 0;
    uint8_t source = 0, count = 0;
    AudioParam params[kMaxAudioParams];
};
inline bool validAudio(const PlayerAudio& a) {
    if (!a.id || a.source >= kMovementSoundCount || a.count > kMaxAudioParams) return false;
    for (uint8_t i = 0; i < a.count; ++i) {
        if (!a.params[i].key || !std::isfinite(a.params[i].value) || std::fabs(a.params[i].value) > 100000.f) return false;
        for (uint8_t j = 0; j < i; ++j) if (a.params[j].key == a.params[i].key) return false;
    }
    return true;
}
}
