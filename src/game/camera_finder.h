#pragma once
// Developer tool (MECMP_DEBUG_KEYS builds, F10): finds the camera's view-projection matrix in
// memory and the game globals that lead to it, and writes both to mecmp.log.
//
// Names above heads need that matrix every frame. Static analysis shows it lives in a render view
// at +0x4a0 (see DebugRenderer_drawPrimitiveBatches in Ghidra), but the view only reaches the code
// through virtual calls. This tool finds it at runtime instead; its log gives the pointer path to
// hardcode.

namespace mecmp::game::camera_finder {

// Mod thread. Takes a few seconds (two passes over the game's heap). Run it in a level.
void run();

} // namespace mecmp::game::camera_finder
