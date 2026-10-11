#include "game/camera_finder.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"

namespace mecmp::game::camera_finder {
namespace {

constexpr uintptr_t kImageBase = 0x140000000;

struct Vec3 { float x, y, z; };

// The camera's view-projection matrix. Seen in the first runs: Frostbite's reversed infinite
// projection, so the clip-z row/column is (0, 0, 0, near) with near = 0.06, and the clip-w one is the
// unit view direction plus an offset (clip.w ~ 0 at the eye). `transposed` = stored column-major
// (clip = M * p, rows as stored); otherwise row vectors (clip = p * M).
bool looksLikeViewProj(const float* m, const Vec3& eye, bool transposed, float& wAtEye) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e5f) return false;
    // at(k, i): coefficient of input i (x, y, z, 1) in clip component k (x, y, z, w)
    auto at = [&](int k, int i) { return transposed ? m[k * 4 + i] : m[i * 4 + k]; };
    // No requirement on the clip-z part: the render view may store its matrix with another depth mapping
    // than the shader copies (those use reversed infinite depth: z row 0,0,0,0.06).
    const float fx = at(3, 0), fy = at(3, 1), fz = at(3, 2);
    const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 0.98f || len > 1.02f) return false;
    const float sx = std::sqrt(at(0, 0) * at(0, 0) + at(0, 1) * at(0, 1) + at(0, 2) * at(0, 2));
    const float sy = std::sqrt(at(1, 0) * at(1, 0) + at(1, 1) * at(1, 1) + at(1, 2) * at(1, 2));
    if (sx < 0.1f || sx > 10.f || sy < 0.1f || sy > 10.f) return false;
    wAtEye = eye.x * fx + eye.y * fy + eye.z * fz + at(3, 3);
    return std::fabs(wAtEye) < 2.5f;
}

uintptr_t toVa(uintptr_t a) { return a - mem::moduleBase() + kImageBase; }

} // namespace

namespace {
// Rows 0-2 orthonormal and row 3 near the eye: a camera transform.
bool looksLikeCameraTransform(const float* m, const Vec3& eye) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    for (int r = 0; r < 3; ++r) {
        const float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (l < 0.97f || l > 1.03f) return false;
    }
    const float d = std::fabs(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]) + std::fabs(m[0] * m[8] + m[1] * m[9] + m[2] * m[10]);
    if (d > 0.05f) return false;
    const float dx = m[12] - eye.x, dy = m[13] - eye.y, dz = m[14] - eye.z;
    return dx * dx + dy * dy + dz * dz < 9.f;
}

// Logs camera transforms and view-projection matrices inside [obj, obj + size).
void dumpObject(const char* what, uintptr_t obj, size_t size, const Vec3& eye) {
    std::vector<uint8_t> b(size);
    if (!obj || !mem::read(obj, b.data(), size)) {
        MLOG("camera: %s %p not readable", what, reinterpret_cast<void*>(obj));
        return;
    }
    MLOG("camera: %s %p, vtable 0x%llx", what, reinterpret_cast<void*>(obj),
         static_cast<unsigned long long>(toVa(*reinterpret_cast<uintptr_t*>(b.data()))));
    for (size_t o = 0; o + 64 <= size; o += 4) {
        const auto* m = reinterpret_cast<const float*>(b.data() + o);
        float w = 0;
        if (looksLikeCameraTransform(m, eye))
            MLOG("camera:   +0x%03llx transform: right %.3f %.3f %.3f / up %.3f %.3f %.3f / fwd %.3f %.3f %.3f / pos %.2f %.2f %.2f",
                 static_cast<unsigned long long>(o), m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
        for (bool t : {false, true})
            if (looksLikeViewProj(m, eye, t, w))
                MLOG("camera:   +0x%03llx view-projection (%s), w(eye) %.2f: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.2f %.2f %.2f %.2f",
                     static_cast<unsigned long long>(o), t ? "col" : "row", w, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                     m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
}
} // namespace

// g_clientCameraViewManagers (0x142415e50) -> +0x08 vector of ClientCameraViewManager* (one per local
// player) -> +0x208 active camera (ClientCameraViewManager_postSimUpdate, 0x14327c2a0).
void run() {
    void* player = level::playerCharacter();
    LinearTransform xf;
    if (!player || !readEntityTransform(player, xf)) {
        MLOG("camera: load into a level first");
        return;
    }
    const Vec3 eye{xf.trans[0], xf.trans[1] + 1.7f, xf.trans[2]};
    MLOG("camera: following g_clientCameraViewManagers (eye about %.1f %.1f %.1f)", eye.x, eye.y, eye.z);
    const uintptr_t managers = mem::readOr<uintptr_t>(rva(0x142415e50));
    uintptr_t vec[2] = {};
    if (!managers || !mem::read(managers + 8, vec, sizeof(vec)) || vec[1] < vec[0] || vec[1] - vec[0] > 64) {
        MLOG("camera: manager list %p not readable", reinterpret_cast<void*>(managers));
        return;
    }
    MLOG("camera: %llu camera view manager(s)", static_cast<unsigned long long>((vec[1] - vec[0]) / 8));
    for (uintptr_t at = vec[0]; at < vec[1]; at += 8) {
        const uintptr_t mgr = mem::readOr<uintptr_t>(at);
        dumpObject("manager", mgr, 0x1200, eye);
        const uintptr_t cam = mgr ? mem::readOr<uintptr_t>(mgr + 0x208) : 0;
        dumpObject("active camera", cam, 0x600, eye);
        float settings[17] = {};
        if (cam && mem::read(cam + 0xac, settings, sizeof(settings)))
            MLOG("camera:   settings +0xac..+0xec: %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | +0xd0: %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
                 settings[0], settings[1], settings[2], settings[3], settings[4], settings[5], settings[6], settings[7],
                 settings[8], settings[9], settings[10], settings[11], settings[12], settings[13], settings[14], settings[15],
                 settings[16]);
    }
    MLOG("camera: done");
}

} // namespace mecmp::game::camera_finder
