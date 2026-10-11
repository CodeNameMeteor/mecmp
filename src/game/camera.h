#pragma once
// The local player's camera, read from the game each frame for name tags.
//
// g_clientCameraViewManagers (0x142415e50) -> +0x08 vector of ClientCameraViewManager* (one per local
// player). A manager keeps the final camera transform at +0x90 and the active camera at +0x208, whose
// +0xac is only the camera's base field of view (55). The manager's +0x80 is the final vertical field of
// view the renderer uses, with the player's FOV setting applied (90 in testing; +0x20 is the aspect).
// (ClientCameraViewManager_postSimUpdate in Ghidra.)

#include "game/camera_math.h"

namespace mecmp::game::camera {

// Any thread (fault-tolerant reads). False between levels or if the values do not look like a camera.
bool read(CameraView& out);

} // namespace mecmp::game::camera
