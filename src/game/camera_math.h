#pragma once
// World -> screen for name tags, markers and the arrows on the screen's edge. Header-only and free of Windows/game code, so it is unit-tested
// on Linux (tests/camera_test.cpp).
//
// Frostbite camera transform: rows right, up, forward, position. The camera looks along MINUS its
// forward row (right-handed: right x up = forward, which points backwards out of the screen); seen
// in game, where using +forward drew points behind the player in front of them. The field of view is
// vertical, in degrees.

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
    const float x = dot(d, c.right), y = dot(d, c.up), z = -dot(d, c.forward); // z = distance ahead
    if (!(z > nearZ) || width <= 0 || height <= 0) return false;
    const float tanY = std::tan(c.fovYDeg * 0.5f * 3.14159265f / 180.f);
    const float tanX = tanY * width / height;
    const float ndcX = x / (z * tanX), ndcY = y / (z * tanY);
    sx = (0.5f + 0.5f * ndcX) * width;
    sy = (0.5f - 0.5f * ndcY) * height;
    depth = z;
    return std::isfinite(sx) && std::isfinite(sy);
}

// How visible something `depth` metres ahead is: full until 40 m, gone by 150 m (name tags, markers).
constexpr float kFadeFullUntil = 40.f;
constexpr float kFadeGoneAt = 150.f;
inline float distanceFade(float depth) {
    if (!(depth > kFadeFullUntil)) return 1.f;
    if (depth >= kFadeGoneAt) return 0.f;
    return 1.f - (depth - kFadeFullUntil) / (kFadeGoneAt - kFadeFullUntil);
}

// Where to draw a pointer to world point `p`: on the point itself while it is on screen, otherwise
// on the screen's edge in its direction.
struct EdgeIndicator {
    bool onScreen = false;
    float x = 0, y = 0;       // pixels: the point, or the spot on the edge (inset by the margin)
    float dirX = 0, dirY = 1; // unit screen direction from the centre towards the point (y grows downwards)
    float distance = 0;       // metres from the camera
};
// `margin` = how far inside the screen the edge spot stays (pixels). False only when nothing can be
// drawn (no screen, or values that are not numbers). A point exactly behind the camera points down.
inline bool edgeIndicator(const CameraView& c, const float p[3], float width, float height, float margin,
                          EdgeIndicator& out) {
    const float halfW = width * 0.5f - margin, halfH = height * 0.5f - margin;
    if (!(halfW > 0) || !(halfH > 0)) return false;
    const float d[3] = {p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2]};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float x = dot(d, c.right), y = dot(d, c.up);
    out.distance = std::sqrt(dot(d, d));
    if (!std::isfinite(out.distance) || !std::isfinite(x) || !std::isfinite(y)) return false;
    // The projection scales x and y by the same factor (pixels per metre at that depth), so the
    // direction on screen is (x, -y) whether the point is ahead of the camera or behind it.
    const float len = std::sqrt(x * x + y * y);
    out.dirX = len > 1e-4f ? x / len : 0.f;
    out.dirY = len > 1e-4f ? -y / len : 1.f;
    float sx = 0, sy = 0, depth = 0;
    if (projectToScreen(c, p, width, height, sx, sy, depth) && sx >= margin && sx <= width - margin && sy >= margin &&
        sy <= height - margin) {
        out.onScreen = true;
        out.x = sx;
        out.y = sy;
        return true;
    }
    // From the centre along the direction until the inset rectangle's border.
    const float ax = std::fabs(out.dirX), ay = std::fabs(out.dirY);
    const float t = ax * halfH > ay * halfW ? halfW / ax : halfH / ay;
    out.onScreen = false;
    out.x = width * 0.5f + out.dirX * t;
    out.y = height * 0.5f + out.dirY * t;
    return true;
}

} // namespace mecmp::game
