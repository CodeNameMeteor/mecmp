#pragma once
// World -> screen for name tags. Header-only and free of Windows/game code, so it is unit-tested
// on Linux (tests/camera_test.cpp).
//
// Frostbite camera transform: rows right, up, forward, position (left-handed: right = up x forward).
// The field of view is vertical, in degrees.

#include <cmath>

namespace mecmp::game {

struct CameraView {
    float right[3], up[3], forward[3], pos[3];
    float fovYDeg = 55.f;
};

// Screen position (pixels, origin top-left) of world point `p`. False if it is behind the camera
// (or closer than `nearZ`). `depth` = distance along the view direction.
inline bool projectToScreen(const CameraView& c, const float p[3], float width, float height, float& sx, float& sy,
                            float& depth, float nearZ = 0.1f) {
    const float d[3] = {p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2]};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float x = dot(d, c.right), y = dot(d, c.up), z = dot(d, c.forward);
    if (!(z > nearZ) || width <= 0 || height <= 0) return false;
    const float tanY = std::tan(c.fovYDeg * 0.5f * 3.14159265f / 180.f);
    const float tanX = tanY * width / height;
    const float ndcX = x / (z * tanX), ndcY = y / (z * tanY);
    sx = (0.5f + 0.5f * ndcX) * width;
    sy = (0.5f - 0.5f * ndcY) * height;
    depth = z;
    return std::isfinite(sx) && std::isfinite(sy);
}

} // namespace mecmp::game
