#pragma once
// The local player's camera, read from the game each frame for name tags.
//
// g_clientCameraViewManagers (0x142415e50) -> +0x08 vector of ClientCameraViewManager* (one per local
// player). A manager keeps the final camera transform at +0x90 and the active camera at +0x208, whose
// +0xac is the camera's base vertical field of view in degrees. The player's FOV setting scales it:
// GameRenderSettings.FovMultiplier (+0x58), or GameRenderSettings.ForceFov (+0x54) replaces it when set.
// (ClientCameraViewManager_postSimUpdate, SettingsManager_getByType in Ghidra.)

#include "game/camera_math.h"

namespace mecmp::game::camera {

// Any thread (fault-tolerant reads). False between levels or if the values do not look like a camera.
bool read(CameraView& out);

// Startup: runtime TypeInfo of GameRenderSettings. Mod thread, about once a second: looks the
// settings object up (SettingsManager_getByType takes the manager's lock, so not per frame).
void setSettingsType(uintptr_t runtimeTypeInfo);
void refreshSettings();

// Values behind the last read, for the log: base FOV, multiplier, forced FOV (0 = off).
void describe(float& baseFov, float& multiplier, float& forced);

} // namespace mecmp::game::camera
