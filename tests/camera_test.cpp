// Linux test of the name-tag projection and the screen-edge pointers (game/camera_math.h).
#include <cmath>
#include <cstdio>

#include "game/camera_math.h"

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static bool near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) < eps; }

int main() {
    using mecmp::game::CameraView;
    using mecmp::game::projectToScreen;
    // Camera at the origin looking down -Z: Frostbite's forward row (+Z here) points backwards.
    CameraView c{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}, 90.f};
    float sx = 0, sy = 0, d = 0;
    const float ahead[3] = {0, 0, -10};
    CHECK(projectToScreen(c, ahead, 1920, 1080, sx, sy, d));
    CHECK(near(sx, 960) && near(sy, 540) && near(d, 10));
    // 90 degree vertical FOV: a point as high as it is far sits at the top edge.
    const float top[3] = {0, 10, -10};
    CHECK(projectToScreen(c, top, 1920, 1080, sx, sy, d) && near(sy, 0) && near(sx, 960));
    // Right of the camera -> right half of the screen; the edge is at tan(fovX/2) = 16/9.
    const float right[3] = {10 * 1920.f / 1080.f, 0, -10};
    CHECK(projectToScreen(c, right, 1920, 1080, sx, sy, d) && near(sx, 1920));
    // Behind the camera: not drawn.
    const float behind[3] = {0, 0, 5};
    CHECK(!projectToScreen(c, behind, 1920, 1080, sx, sy, d));
    // The camera from the game log (forward row -0.108 0.013 -0.994): it looks along +0.108 -0.013 +0.994.
    CameraView g{{-0.994f, -0.001f, 0.108f}, {0.001f, 1.f, 0.013f}, {-0.108f, 0.013f, -0.994f}, {56.42f, 68.52f, 168.73f}, 55.f};
    const float infront[3] = {56.42f + 0.108f * 20, 68.52f - 0.013f * 20, 168.73f + 0.994f * 20};
    CHECK(projectToScreen(g, infront, 1920, 1080, sx, sy, d) && near(sx, 960, 2) && near(sy, 540, 2) && near(d, 20, 0.1f));
    // A point to the camera's right (+right vector) lands right of centre.
    const float toRight[3] = {infront[0] - 0.994f * 3, infront[1], infront[2] + 0.108f * 3};
    CHECK(projectToScreen(g, toRight, 1920, 1080, sx, sy, d) && sx > 1000);

    // --- Distance fade ---
    using mecmp::game::distanceFade;
    CHECK(distanceFade(0) == 1.f && distanceFade(40) == 1.f && distanceFade(150) == 0.f && distanceFade(1000) == 0.f);
    CHECK(near(distanceFade(95), 0.5f, 0.01f));

    // --- Pointers on the screen's edge (markers, off-screen players) ---
    using mecmp::game::EdgeIndicator;
    using mecmp::game::edgeIndicator;
    const float W = 1920, H = 1080, M = 40;
    EdgeIndicator e;
    // On screen: the point itself.
    CHECK(edgeIndicator(c, ahead, W, H, M, e) && e.onScreen && near(e.x, 960) && near(e.y, 540) && near(e.distance, 10));
    // Ahead but far to the right: the right edge, at the same height as the centre, pointing right.
    const float farRight[3] = {100, 0, -10};
    CHECK(edgeIndicator(c, farRight, W, H, M, e) && !e.onScreen && near(e.x, W - M) && near(e.y, 540));
    CHECK(near(e.dirX, 1, 0.01f) && near(e.dirY, 0, 0.01f));
    // Far above: the top edge (screen y grows downwards, so the direction's y is negative).
    const float farUp[3] = {0, 100, -10};
    CHECK(edgeIndicator(c, farUp, W, H, M, e) && !e.onScreen && near(e.y, M) && near(e.x, 960) && near(e.dirY, -1, 0.01f));
    // Left and below.
    const float farLeft[3] = {-100, 0, -10};
    CHECK(edgeIndicator(c, farLeft, W, H, M, e) && !e.onScreen && near(e.x, M) && near(e.y, 540));
    const float farDown[3] = {0, -100, -10};
    CHECK(edgeIndicator(c, farDown, W, H, M, e) && !e.onScreen && near(e.y, H - M) && near(e.x, 960));
    // Behind the camera and to the left: still the left edge (not mirrored).
    const float behindLeft[3] = {-5, 0, 20};
    CHECK(edgeIndicator(c, behindLeft, W, H, M, e) && !e.onScreen && near(e.x, M) && near(e.y, 540) && e.dirX < -0.99f);
    // Behind and up-right: a corner direction, clamped onto the border of the inset rectangle.
    const float behindUpRight[3] = {5, 5, 20};
    CHECK(edgeIndicator(c, behindUpRight, W, H, M, e) && !e.onScreen && e.dirX > 0 && e.dirY < 0);
    CHECK(e.x >= M - 0.5f && e.x <= W - M + 0.5f && e.y >= M - 0.5f && e.y <= H - M + 0.5f);
    CHECK(near(e.y, M)); // 45 degrees on a wide screen meets the top edge first
    // Exactly behind: no direction of its own, so it points down.
    CHECK(edgeIndicator(c, behind, W, H, M, e) && !e.onScreen && near(e.x, 960) && near(e.y, H - M) && e.dirY == 1.f);
    // Just inside the margin band counts as off screen (the spot stays inside the margin).
    const float nearEdge[3] = {10 * 1920.f / 1080.f * 0.99f, 0, -10};
    CHECK(edgeIndicator(c, nearEdge, W, H, M, e) && !e.onScreen && near(e.x, W - M));
    // The real camera: a point to its right and behind it points right.
    const float gBehindRight[3] = {56.42f - 0.108f * 20 - 0.994f * 5, 68.52f, 168.73f - 0.994f * 20 + 0.108f * 5};
    CHECK(edgeIndicator(g, gBehindRight, W, H, M, e) && !e.onScreen && e.dirX > 0.9f && near(e.x, W - M));
    // Nothing to draw on: no screen, a margin that leaves no room, or a position that is not a number.
    CHECK(!edgeIndicator(c, ahead, 0, 0, M, e));
    CHECK(!edgeIndicator(c, ahead, 60, 60, 40, e));
    const float nan3[3] = {std::nanf(""), 0, 0};
    CHECK(!edgeIndicator(c, nan3, W, H, M, e));
    if (g_failures == 0) std::printf("camera tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
