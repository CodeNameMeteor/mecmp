#include "game/camera.h"

#include <cmath>

#include "game/engine.h"

namespace mecmp::game::camera {

bool read(CameraView& out) {
    const uintptr_t managers = mem::readOr<uintptr_t>(rva(kClientCameraViewManagers));
    const uintptr_t first = managers ? mem::readOr<uintptr_t>(managers + CamMgrs_List) : 0;
    const uintptr_t mgr = first ? mem::readOr<uintptr_t>(first) : 0; // local player 0
    float m[16];
    if (!mgr || !mem::read(mgr + CamMgr_Transform, m, sizeof(m))) return false;
    for (float v : m)
        if (!std::isfinite(v)) return false;
    // rows 0-2 must be unit vectors (a rotation), or this is not the camera we think it is
    for (int r = 0; r < 3; ++r) {
        const float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (l < 0.95f || l > 1.05f) return false;
    }
    for (int i = 0; i < 3; ++i) {
        out.right[i] = m[i];
        out.up[i] = m[4 + i];
        out.forward[i] = m[8 + i];
        out.pos[i] = m[12 + i];
    }
    const uintptr_t active = mem::readOr<uintptr_t>(mgr + CamMgr_ActiveCamera);
    const float fov = active ? mem::readOr<float>(active + Camera_FovYDeg, 0.f) : 0.f;
    out.fovYDeg = std::isfinite(fov) && fov > 20.f && fov < 150.f ? fov : 55.f;
    return true;
}

} // namespace mecmp::game::camera
