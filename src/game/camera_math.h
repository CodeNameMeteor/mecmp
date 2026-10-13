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

// How visible something `depth` metres ahead is: full until `fullUntil`, gone by `goneAt`. Name tags:
// 40 m to 150 m. Markers: 50 m to 300 m, as far as an aimed marker reaches (game/ping_point.cpp).
constexpr float kFadeFullUntil = 40.f;
constexpr float kFadeGoneAt = 150.f;
constexpr float kMarkerFadeFullUntil = 50.f;
constexpr float kMarkerFadeGoneAt = 300.f;
inline float distanceFade(float depth, float fullUntil = kFadeFullUntil, float goneAt = kFadeGoneAt) {
    if (!(depth > fullUntil)) return 1.f;
    if (depth >= goneAt) return 0.f;
    return 1.f - (depth - fullUntil) / (goneAt - fullUntil);
}

// The marker wheel: which of `n` equal wedges the offset (dx, dy) from its centre points at, in screen
// pixels with y down. Wedge 0 is centred straight up, the others follow clockwise. -1 inside the dead
// zone (nothing chosen), and for a NaN or n <= 0.
inline int radialSlot(float dx, float dy, float deadZone, int n) {
    if (n <= 0 || !(dx * dx + dy * dy > deadZone * deadZone)) return -1;
    const float twoPi = 6.28318531f;
    float a = std::atan2(dx, -dy); // 0 = up, increasing clockwise
    if (a < 0.f) a += twoPi;
    const int i = static_cast<int>(a / (twoPi / static_cast<float>(n)) + 0.5f);
    return i % n;
}

// The flash a marker makes when it arrives, so a new or moved one is noticed: the marker blinks
// white twice and swells and settles, while rings spread from it and fade. One pass of kPulseMs,
// then it stays at rest.
constexpr unsigned kPulseMs = 1800;
struct Pulse {
    float ring = 1.f;  // 0 = rings just started, 1 = finished (nothing to draw)
    float scale = 1.f; // multiplier for the marker's size
    float flash = 0.f; // 0..1: how far the marker's colour is blended to white (two blinks)
};
inline Pulse pingPulse(unsigned ageMs) {
    Pulse p;
    if (ageMs >= kPulseMs) return p;
    const float t = static_cast<float>(ageMs) / static_cast<float>(kPulseMs);
    // A blink: a smooth bump of half-width w around c (0 outside it).
    auto bump = [](float x, float c, float w) {
        const float d = std::fabs(x - c);
        return d >= w ? 0.f : 0.5f * (1.f + std::cos(3.14159265f * d / w));
    };
    // Two short blinks 200 ms apart (peaks at 90 ms and 288 ms), clearly separate.
    p.flash = std::fmax(bump(t, 0.05f, 0.04f), bump(t, 0.16f, 0.04f));
    p.ring = 1.f - (1.f - t) * (1.f - t); // fast at first, then settling
    p.scale = 1.f + (0.5f + 0.25f * p.flash) * (1.f - t);
    return p;
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
