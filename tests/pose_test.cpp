// Tests for game/pose_math.h:
//   g++ -std=c++17 -Isrc tests/pose_test.cpp -o pose_test && ./pose_test
#include <cmath>
#include <cstdio>

#include "game/pose_math.h"

using namespace mecmp::game;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static bool near3(const float* a, const float* b, float eps) {
    return std::fabs(a[0] - b[0]) < eps && std::fabs(a[1] - b[1]) < eps && std::fabs(a[2] - b[2]) < eps;
}

int main() {
    // Identity bone 0: bone 1 is the position itself.
    {
        const float b0[8] = {0, 0, 0, 1, 0, 0, 0, 1};
        float b1[8] = {0, 0, 0, 1, 0, 0, 0, 1};
        const float pos[3] = {1114.15f, 89.62f, -1.33f};
        anchorBone1(b0, b1, pos);
        CHECK(near3(b1, pos, 1e-4f));
    }
    // The bug: a 0.1 degree tilt in bone 0 moves a body 1 km from the origin by ~2 m...
    const float a = 0.1f * 3.14159265f / 180.f;
    const float b0[8] = {0.02f, -0.01f, 0.03f, 1, std::sin(a / 2) * 0.6f, std::sin(a / 2) * 0.8f, 0, std::cos(a / 2)};
    const float pos[3] = {1114.15f, 89.62f, -1.33f};
    {
        float b1[8] = {pos[0], pos[1], pos[2], 1, 0, 0, 0, 1};
        float placed[3];
        replayPlacement(b0, b1, placed);
        const float dx = placed[0] - pos[0], dy = placed[1] - pos[1], dz = placed[2] - pos[2];
        CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) > 0.5f);
    }
    // ...and anchored, it lands on the position.
    {
        float b1[8] = {0, 0, 0, 1, 0, 0, 0, 1};
        anchorBone1(b0, b1, pos);
        float placed[3];
        replayPlacement(b0, b1, placed);
        CHECK(near3(placed, pos, 2e-3f));
    }
    // A bone 0 with a scale (translation w) and an unnormalised rotation still lands exactly.
    {
        const float b0s[8] = {1.f, 2.f, -3.f, 0.98f, 0.1f, 0.2f, -0.05f, 1.02f};
        float b1[8] = {};
        anchorBone1(b0s, b1, pos);
        float placed[3];
        replayPlacement(b0s, b1, placed);
        CHECK(near3(placed, pos, 5e-3f));
    }
    // A degenerate bone 0 falls back to the position as is.
    {
        const float zero[8] = {};
        float b1[8] = {};
        anchorBone1(zero, b1, pos);
        CHECK(near3(b1, pos, 1e-6f));
    }
    // Folded: bone 1 is the position as is AND the game's formula gives it, with the same orientation.
    {
        float f0[8], f1[8] = {5, 6, 7, 1, 0.1f, 0.2f, 0.3f, 0.927f};
        for (int j = 0; j < 8; ++j) f0[j] = b0[j];
        float before[4];
        quatMul(&b0[4], &f1[4], before);
        foldBone0(f0, f1, pos);
        CHECK(near3(f1, pos, 1e-6f));
        float placed[3];
        replayPlacement(f0, f1, placed);
        CHECK(near3(placed, pos, 1e-3f));
        float after[4];
        quatMul(&f0[4], &f1[4], after);
        const float nb = std::sqrt(before[0] * before[0] + before[1] * before[1] + before[2] * before[2] + before[3] * before[3]);
        float dot = 0;
        for (int j = 0; j < 4; ++j) dot += before[j] / nb * after[j];
        CHECK(std::fabs(dot) > 0.99999f);
    }
    if (g_failures == 0) std::printf("pose tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
