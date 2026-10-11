#include "game/camera.h"

#include <atomic>
#include <cmath>

#include "game/engine.h"

namespace mecmp::game::camera {
namespace {
std::atomic<uintptr_t> g_settingsType{0};
std::atomic<uintptr_t> g_settings{0}; // GameRenderSettings object
std::atomic<float> g_lastBase{0}, g_lastMultiplier{0}, g_lastForced{0};
} // namespace

void setSettingsType(uintptr_t ti) { g_settingsType = ti; }

void refreshSettings() {
    const uintptr_t ti = g_settingsType.load();
    const uintptr_t mgr = mem::readOr<uintptr_t>(rva(kSettingsManager));
    if (!ti || !mgr) return;
    using GetByType = uintptr_t (*)(uintptr_t, uintptr_t);
    g_settings = reinterpret_cast<GetByType>(rva(Fn_SettingsManager_getByType))(mgr, ti);
}

void describe(float& baseFov, float& multiplier, float& forced) {
    baseFov = g_lastBase;
    multiplier = g_lastMultiplier;
    forced = g_lastForced;
}

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
    float fov = active ? mem::readOr<float>(active + Camera_FovYDeg, 0.f) : 0.f;
    if (!std::isfinite(fov) || fov < 20.f || fov > 150.f) fov = 55.f;
    g_lastBase = fov;
    float multiplier = 1.f, forced = 0.f;
    if (const uintptr_t s = g_settings.load()) {
        multiplier = mem::readOr<float>(s + GameRenderSettings_FovMultiplier, 1.f);
        forced = mem::readOr<float>(s + GameRenderSettings_ForceFov, 0.f);
    }
    g_lastMultiplier = multiplier;
    g_lastForced = forced;
    if (std::isfinite(forced) && forced > 1.f && forced < 170.f) fov = forced;
    else if (std::isfinite(multiplier) && multiplier > 0.2f && multiplier < 4.f) fov *= multiplier;
    out.fovYDeg = std::fmin(fov, 170.f);
    return true;
}

} // namespace mecmp::game::camera
