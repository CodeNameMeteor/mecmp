#pragma once
// Developer tool (MECMP_DEBUG_KEYS builds, F10): follows the camera path found in Ghidra
// (g_clientCameraViewManagers -> ClientCameraViewManager -> +0x208 active camera) and logs every
// camera transform and view-projection matrix inside those objects, with their offsets.

namespace mecmp::game::camera_finder {

// Mod thread. Run it in a level.
void run();

} // namespace mecmp::game::camera_finder
