#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace mecmp::net {
// Only bounded indices, event hashes and scalar inputs travel over the network.
// Keep the first 17 indices unchanged for the existing movement packets.
inline constexpr const char* kPlayerSounds[] = {
    "ME_Sound/Character/Faith/Footsteps/Faith_Footplant",
    "ME_Sound/Character/Faith/Handsteps/Faith_Handplant", "ME_Sound/Character/Faith/Movement",
    "ME_Sound/Character/Faith/Faith_LandIntoHang", "ME_Sound/Character/Faith/Handsteps/PipeSlide",
    "ME_Sound/Character/Faith/Faith_HeadAgainstCeiling", "ME_Sound/Character/Faith/Footsteps/TarpaulinLanding",
    "ME_Sound/Character/Faith/Faith_LandOnPipe", "ME_Sound/Character/Faith/Handsteps/PipeRelease",
    "ME_Sound/Character/Faith/Faith_LandIntoLadder", "ME_Sound/Character/Faith/Faith_LandIntoPipe",
    "ME_Sound/Character/Faith/Faith_HandsAgainstWall", "ME_Sound/Character/Faith/Faith_CharacterAgainstWall",
    "ME_Sound/Character/Faith/Footsteps/Metal/MetalGantryCreaks",
    "ME_Sound/Character/Faith/Handsteps/MonkeyBar_Swing", "ME_Sound/Character/Faith/Faith_LandIntoBar",
    "ME_Sound/Character/Faith/Handsteps/LadderClimb",
    "ME_Sound/Character/Faith/Vocal/Emotes/Faith_Breathing_Emotes",
    "ME_Sound/Character/Faith/Melee/Faith_Combat_Melee",
    "ME_Sound/Character/AI/AI_Mario_Impact",
    "ME_Sound/Character/Faith/Melee/Faith_Damage_Impacts_Combat",
    "ME_Sound/Weapons/FlyBys/Faith_Nearmiss_Flyby",
    "ME_Sound/Character/Faith/FX/FaithCriticalHealth",
    "ME_Sound/Character/Faith/Melee/Impacts/Faith_Nearmiss_Damage",
    "ME_Sound/Character/Faith/Melee/Faith_Combat_DroneKick"
};
inline constexpr const char* kPlayerVocalWaves[] = {
    "ME_Sound/Character/Faith/Vocal/ME_Faith_Bump",
    "ME_Sound/Character/Faith/Vocal/ME_FaithLedgeStopExhale",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_Scrape",
    "ME_Sound/Character/Faith/Vocal/ME_FaithJumpLight",
    "ME_Sound/Character/Faith/Vocal/Breath/ME_FaithDeathFallBreathing_Stereo",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_DamageLite",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_Damage",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_DamageInjuredBreathTail",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_StrainLite",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_PunchJab",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_PunchFull",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_LandSoft",
    "ME_Sound/Character/Faith/Vocal/ME_Faith_LandHard",
    "ME_Sound/Character/Faith/Vocal/ME_FaithBreathMouthFastIn_Plus",
    "ME_Sound/Character/Faith/Vocal/ME_FaithHurtBreathing"
};
constexpr size_t kPlayerSoundCount = sizeof(kPlayerSounds) / sizeof(*kPlayerSounds);
constexpr size_t kPlayerVocalCount = sizeof(kPlayerVocalWaves) / sizeof(*kPlayerVocalWaves);
constexpr size_t kPlayerAudioSourceCount = kPlayerSoundCount + kPlayerVocalCount;
static_assert(kPlayerAudioSourceCount <= 256, "audio source indices fit in one byte");
constexpr uint32_t kVocalTrigger = 0x9b59cebd, kVocalCancel = 0xa1e48be3;
constexpr int kMaxPlayerAudioPerSecond = 64;
constexpr bool vocalSource(uint8_t source) { return source >= kPlayerSoundCount && source < kPlayerAudioSourceCount; }
constexpr size_t kMaxAudioParams = 16;
struct AudioParam { uint32_t key = 0; float value = 0; };
struct PlayerAudio {
    uint32_t id = 0, timeMs = 0, event = 0;
    uint8_t source = 0, count = 0;
    AudioParam params[kMaxAudioParams];
};
inline bool validAudio(const PlayerAudio& a) {
    if (!a.id || a.source >= kPlayerAudioSourceCount || a.count > kMaxAudioParams) return false;
    if (vocalSource(a.source)) return !a.count && (a.event == kVocalTrigger || a.event == kVocalCancel);
    for (uint8_t i = 0; i < a.count; ++i) {
        if (!a.params[i].key || !std::isfinite(a.params[i].value) || std::fabs(a.params[i].value) > 100000.f) return false;
        for (uint8_t j = 0; j < i; ++j) if (a.params[j].key == a.params[i].key) return false;
    }
    return true;
}
}
