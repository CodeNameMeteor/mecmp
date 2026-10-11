// Linux test of the name-tag projection (game/camera_math.h).
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
    if (g_failures == 0) std::printf("camera tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
