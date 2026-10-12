#pragma once
// Where the game puts a replayed body, and how to make that the player's position. Header-only and
// free of Windows/game code, so it is unit-tested on Linux (tests/pose_test.cpp).
//
// The ghost's client replay update (PamClientReplayPlayerComponent_update, 0x1439501a0) places the
// character at
//     bone0.t + bone0.t.w * rotate(bone0.q, bone1.t)
// (bone = {translation x,y,z,w; rotation x,y,z,w}). Bone 1 (the AITrajectory bone) holds the world
// position; bone 0 is the identity, but after the wire's rotation compression (10 bits per
// component) it decodes as a ~0.14 degree tilt. Writing the player's position into
// bone 1 as is put the body off by that small angle times the distance from the level's origin, so
// a few metres away from it the body floated or sank (more the higher the player). anchorBone1()
// writes bone 1 so that the game's formula gives the position wanted.

#include <cmath>

namespace mecmp::game {

// Hamilton product, quaternions as x,y,z,w.
inline void quatMul(const float* a, const float* b, float* out) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

// q * v * conj(q), as the game computes it (for a unit q this is the rotation of v by q).
inline void quatSandwich(const float* q, const float* v, float* out) {
    const float vq[4] = {v[0], v[1], v[2], 0.f};
    const float c[4] = {-q[0], -q[1], -q[2], q[3]};
    float t[4], r[4];
    quatMul(q, vq, t);
    quatMul(t, c, r);
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

// The game's placement of a replayed body from bones 0 and 1 (8 floats each).
inline void replayPlacement(const float* bone0, const float* bone1, float* out) {
    float r[3];
    quatSandwich(&bone0[4], bone1, r);
    for (int j = 0; j < 3; ++j) out[j] = bone0[j] + bone0[3] * r[j];
}

// Sets bone 1's translation so that replayPlacement(bone0, bone1) == pos. Leaves bone 1 alone
// (apart from its translation set to pos) if bone 0 cannot be inverted.
inline void anchorBone1(const float* bone0, float* bone1, const float* pos) {
    const float* q = &bone0[4];
    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    const float s = bone0[3] * n2; // q v q* scales by |q|^2
    if (!(std::fabs(s) > 1e-6f) || !std::isfinite(s)) {
        bone1[0] = pos[0]; bone1[1] = pos[1]; bone1[2] = pos[2]; bone1[3] = 1.f;
        return;
    }
    const float d[3] = {pos[0] - bone0[0], pos[1] - bone0[1], pos[2] - bone0[2]};
    const float c[4] = {-q[0], -q[1], -q[2], q[3]};
    float r[3];
    quatSandwich(c, d, r); // conj(q) d q = |q|^2 R^-1 d
    for (int j = 0; j < 3; ++j) bone1[j] = r[j] / (s * n2);
    bone1[3] = 1.f;
}

// Better: fold bone 0 into bone 1 and leave bone 0 neutral. The client update places the body with
// the formula above, but something else (seen while the game is paused, when that update does not
// run) takes bone 1 as the position as is; with anchorBone1() alone the two disagreed by bone 0's
// tilt times the distance from the origin, and the body jittered between them. With bone 0 = identity
// and bone 1 = {pos, bone0.q * bone1.q} both give the same position and orientation. Bone 0's own
// rotation is a fraction of a degree, so the skeleton under it moves by millimetres at most.
inline void foldBone0(float* bone0, float* bone1, const float* pos) {
    float q[4];
    quatMul(&bone0[4], &bone1[4], q);
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-6f && std::isfinite(n)) {
        for (int j = 0; j < 4; ++j) bone1[4 + j] = q[j] / n;
    }
    bone1[0] = pos[0]; bone1[1] = pos[1]; bone1[2] = pos[2]; bone1[3] = 1.f;
    const float identity[8] = {0, 0, 0, 1, 0, 0, 0, 1};
    for (int j = 0; j < 8; ++j) bone0[j] = identity[j];
}

} // namespace mecmp::game
