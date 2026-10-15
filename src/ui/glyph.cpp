#include "ui/glyph.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace mecmp::ui {
namespace {

constexpr int kMaxPoints = 32;
// The shield's accent part is the shield again, this much of its size (the main color shows as a rim).
constexpr float kShieldScale = 0.54f;
// The heart's accent part is the heart pulled in by this much on every side (units of r).
constexpr float kHeartRim = 0.33f;
// The heart: two round lobes (circles of radius R at (+-lobeX, lobeY)) and straight sides down to the tip.
constexpr float kHeartR = 0.5f, kHeartLobeX = 0.42f, kHeartLobeY = -0.25f, kHeartTipY = 0.95f;

ImU32 dark(float alpha, int base = 220) { return IM_COL32(0, 0, 0, static_cast<int>(base * std::clamp(alpha, 0.f, 1.f))); }

// A closed outline filled with `fill`. Points are in units of r around c (y down); ImGui wants a
// clockwise winding for filled shapes, so the order is flipped when it is not. `outline` = false
// leaves the dark edge out (for a piece of a bigger shape).
void shape(ImDrawList* dl, ImVec2 c, float r, const ImVec2* unit, int n, ImU32 fill, float alpha, bool convex,
           bool outline = true) {
    ImVec2 pts[kMaxPoints];
    float area = 0.f;
    for (int i = 0; i < n; ++i) {
        const ImVec2 a = unit[i], b = unit[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    for (int i = 0; i < n; ++i) {
        const ImVec2 u = unit[area >= 0.f ? i : n - 1 - i]; // positive area in y-down space = clockwise
        pts[i] = ImVec2(c.x + u.x * r, c.y + u.y * r);
    }
    if (convex) dl->AddConvexPolyFilled(pts, n, fill);
    else dl->AddConcavePolyFilled(pts, n, fill);
    if (outline) dl->AddPolyline(pts, n, dark(alpha), ImDrawFlags_Closed, 1.5f);
}

// The same, with the points turned by `turns` quarter turns clockwise first.
void turned(ImDrawList* dl, ImVec2 c, float r, const ImVec2* unit, int n, int turns, ImU32 fill, float alpha, bool convex) {
    ImVec2 u[kMaxPoints];
    for (int i = 0; i < n; ++i) {
        ImVec2 p = unit[i];
        for (int t = 0; t < (turns & 3); ++t) p = ImVec2(-p.y, p.x);
        u[i] = p;
    }
    shape(dl, c, r, u, n, fill, alpha, convex);
}

void rect(ImDrawList* dl, ImVec2 c, float r, float x0, float y0, float x1, float y1, ImU32 fill, float alpha) {
    const ImVec2 u[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    shape(dl, c, r, u, 4, fill, alpha, true);
}

// Only the dark edge of the closed outline `unit` (the fill went on first, in pieces).
void outlinePoly(ImDrawList* dl, ImVec2 c, float r, const ImVec2* unit, int n, float alpha) {
    ImVec2 pts[kMaxPoints];
    for (int i = 0; i < n; ++i) pts[i] = ImVec2(c.x + unit[i].x * r, c.y + unit[i].y * r);
    dl->AddPolyline(pts, n, dark(alpha), ImDrawFlags_Closed, 1.5f);
}

// An icon whose ends are the accent: the shape `u` in `fill`, the `ends` (quads, each as long as the arm
// is wide) over it in `accent`, then the shape's dark edge once on top.
void tipped(ImDrawList* dl, ImVec2 c, float r, const ImVec2* u, int n, bool convex, const ImVec2 (*ends)[4], int count,
            ImU32 fill, ImU32 accent, float alpha) {
    shape(dl, c, r, u, n, fill, alpha, convex, false);
    for (int i = 0; i < count; ++i) shape(dl, c, r, ends[i], 4, accent, alpha, true, false);
    outlinePoly(dl, c, r, u, n, alpha);
}

// The shape `u` drawn again in `accent`, scaled by `scale` about `k` and without the dark edge. `k` is a
// point that sees the whole shape (the shape is star-shaped about it), so the copy always lies inside the
// original: the same form, smaller.
void scaledAccent(ImDrawList* dl, ImVec2 c, float r, const ImVec2* u, int n, ImVec2 k, float scale, ImU32 accent, float alpha,
                  bool convex) {
    ImVec2 in[kMaxPoints];
    for (int i = 0; i < n; ++i) in[i] = ImVec2(k.x + (u[i].x - k.x) * scale, k.y + (u[i].y - k.y) * scale);
    shape(dl, c, r, in, n, accent, alpha, convex, false);
}

// A heart: two round lobes and straight sides down to the tip, nothing curling over at the top.
int heartPoints(ImVec2* out) {
    constexpr float R = kHeartR, lobeX = kHeartLobeX, lobeY = kHeartLobeY, tipY = kHeartTipY;
    constexpr int k = 10;
    const float cleft = std::atan2(-std::sqrt(R * R - lobeX * lobeX), -lobeX); // where the lobes meet
    const float dx = -lobeX, dy = tipY - lobeY;
    const float tangent = std::atan2(dy, dx) - std::acos(R / std::sqrt(dx * dx + dy * dy)); // the side leaves the lobe here
    ImVec2 right[k + 1];
    for (int i = 0; i <= k; ++i) {
        const float a = cleft + (tangent - cleft) * static_cast<float>(i) / k;
        right[i] = ImVec2(lobeX + R * std::cos(a), lobeY + R * std::sin(a));
    }
    int n = 0;
    for (int i = 0; i <= k; ++i) out[n++] = right[i];
    out[n++] = ImVec2(0.f, tipY);
    for (int i = k; i >= 1; --i) out[n++] = ImVec2(-right[i].x, right[i].y);
    return n;
}

// The heart pulled in by `d` on every side (so every margin is the same): the lobes are arcs of radius
// R - d about the same centers, the notch between them a sharp 90 degree corner (two straight lines at 45
// degrees, each touching a lobe), the sides run parallel to the outer ones and meet on the axis
// `d / sin(side angle)` above the tip.
int heartInnerPoints(float d, ImVec2* out) {
    constexpr float R = kHeartR, lobeX = kHeartLobeX, lobeY = kHeartLobeY, tipY = kHeartTipY;
    constexpr int kLobe = 8;
    constexpr float kNotchAngle = -2.3561945f; // -135 degrees: where a line going down-left at 45 degrees touches the lobe
    const float cleft = kNotchAngle;
    const float dx = -lobeX, dy = tipY - lobeY;
    const float tangent = std::atan2(dy, dx) - std::acos(R / std::sqrt(dx * dx + dy * dy));
    const float sideX = lobeX + R * std::cos(tangent), sideY = lobeY + R * std::sin(tangent); // where the side leaves the lobe
    const float sinSide = sideX / std::sqrt(sideX * sideX + (tipY - sideY) * (tipY - sideY));
    const float tipIn = tipY - d / sinSide;
    const float rIn = R - d;
    ImVec2 right[kLobe + 1];
    int m = 0;
    for (int i = 0; i <= kLobe; ++i) { // over the right lobe, from where the notch's line touches it to where the side leaves it
        const float a = cleft + (tangent - cleft) * static_cast<float>(i) / kLobe;
        right[m++] = ImVec2(lobeX + rIn * std::cos(a), lobeY + rIn * std::sin(a));
    }
    // The notch: the line from the touching point goes down towards the axis at 45 degrees, the mirrored one
    // from the left lobe comes the same way, so they meet at a right angle on the axis.
    const float notchY = right[0].y + right[0].x;
    int n = 0;
    out[n++] = ImVec2(0.f, notchY);
    for (int i = 0; i < m; ++i) out[n++] = right[i];
    out[n++] = ImVec2(0.f, tipIn);
    for (int i = m - 1; i >= 0; --i) out[n++] = ImVec2(-right[i].x, right[i].y); // the left half, mirrored
    return n;
}

void disc(ImDrawList* dl, ImVec2 c, float radius, ImU32 fill, float alpha) {
    dl->AddCircleFilled(c, radius, fill);
    dl->AddCircle(c, radius, dark(alpha), 0, 1.5f);
}

void ring(ImDrawList* dl, ImVec2 c, float radius, float width, ImU32 fill, float alpha) {
    dl->AddCircle(c, radius, dark(alpha), 0, width + 3.f);
    dl->AddCircle(c, radius, fill, 0, width);
}

// A line with a dark edge, from a to b in units of r around c.
void stroke(ImDrawList* dl, ImVec2 c, float r, ImVec2 a, ImVec2 b, float width, ImU32 fill, float alpha) {
    const ImVec2 p(c.x + a.x * r, c.y + a.y * r), q(c.x + b.x * r, c.y + b.y * r);
    dl->AddLine(p, q, dark(alpha), width + 3.f);
    dl->AddLine(p, q, fill, width);
}

// The game's own arrow, pointing up: split down the middle into two halves. `turns` quarter turns
// clockwise give right, down and left. The half that is on the right of the up arrow, the left of the
// down arrow, the top of the left one and the bottom of the right one (side +1) is drawn in `accent`.
void splitArrow(ImDrawList* dl, ImVec2 c, float r, int turns, ImU32 fill, float alpha, ImU32 accent) {
    for (float side : {-1.f, 1.f}) {
        const ImVec2 u[5] = {{0.07f * side, -1.f}, {0.67f * side, 0.07f}, {0.37f * side, 0.07f}, {0.37f * side, 1.f}, {0.07f * side, 1.f}};
        turned(dl, c, r, u, 5, turns, side > 0.f ? accent : fill, alpha, false);
    }
}

// A plus sign with arms of half-width w; turned by 45 degrees it is the cross. The end of each arm, half a
// square (as long as the arm is wide, then cut to half of that), is the accent.
void plus(ImDrawList* dl, ImVec2 c, float r, bool diagonal, ImU32 fill, ImU32 accent, float alpha) {
    constexpr float w = 0.26f;
    const float l = diagonal ? 1.05f : 0.9f;
    ImVec2 u[12] = {{-w, -l}, {w, -l}, {w, -w}, {l, -w}, {l, w}, {w, w}, {w, l}, {-w, l}, {-w, w}, {-l, w}, {-l, -w}, {-w, -w}};
    ImVec2 ends[4][4];
    for (int k = 0; k < 4; ++k) { // the top end, turned by k quarter turns
        ImVec2 q[4] = {{-w, -l}, {w, -l}, {w, -l + w}, {-w, -l + w}};
        for (ImVec2& p : q)
            for (int t = 0; t < k; ++t) p = ImVec2(-p.y, p.x);
        for (int i = 0; i < 4; ++i) ends[k][i] = q[i];
    }
    if (diagonal) {
        for (ImVec2& p : u) p = ImVec2((p.x - p.y) * 0.70710678f, (p.x + p.y) * 0.70710678f);
        for (auto& q : ends)
            for (ImVec2& p : q) p = ImVec2((p.x - p.y) * 0.70710678f, (p.x + p.y) * 0.70710678f);
    }
    tipped(dl, c, r, u, 12, false, ends, 4, fill, accent, alpha);
}
} // namespace

void drawGlyph(ImDrawList* dl, uint8_t glyph, ImVec2 c, float r, ImU32 fill, float alpha, ImU32 accent) {
    const ImU32 ink = dark(alpha, 235);
    switch (glyph) {
    case 1: { // chevron pointing down: "go there", with a smaller one above it
        const ImVec2 u[6] = {{-0.9f, -0.75f}, {-0.9f, -0.15f}, {0.f, 0.75f}, {0.9f, -0.15f}, {0.9f, -0.75f}, {0.f, 0.15f}};
        ImVec2 small[6]; // half size, in the notch of the big one: its tip a small gap above the notch's corner
        for (int i = 0; i < 6; ++i) small[i] = ImVec2(u[i].x * 0.5f, u[i].y * 0.5f - 0.35f);
        shape(dl, c, r, u, 6, fill, alpha, false);
        shape(dl, c, r, small, 6, accent, alpha, false);
        break;
    }
    case 2: { // danger: a triangle with an exclamation mark in the accent, edged dark
        const ImVec2 u[3] = {{0.f, -1.f}, {0.95f, 0.8f}, {-0.95f, 0.8f}};
        shape(dl, c, r, u, 3, fill, alpha, true);
        const float w = std::max(2.f, 0.22f * r), dot = std::max(1.5f, 0.12f * r);
        const ImVec2 top(c.x, c.y - 0.3f * r), low(c.x, c.y + 0.22f * r), at(c.x, c.y + 0.52f * r);
        dl->AddLine(top, low, ink, w + 2.f);
        dl->AddCircleFilled(at, dot + 1.f, ink);
        dl->AddLine(top, low, accent, w);
        dl->AddCircleFilled(at, dot, accent);
        break;
    }
    case 3: // wait: two bars, the top and the bottom of each (half a square) the accent
        for (float side : {-1.f, 1.f}) {
            const float x0 = side < 0.f ? -0.62f : 0.18f, x1 = x0 + 0.44f;
            const ImVec2 bar[4] = {{x0, -0.8f}, {x1, -0.8f}, {x1, 0.8f}, {x0, 0.8f}};
            const ImVec2 ends[2][4] = {{{x0, -0.8f}, {x1, -0.8f}, {x1, -0.58f}, {x0, -0.58f}},
                                       {{x0, 0.58f}, {x1, 0.58f}, {x1, 0.8f}, {x0, 0.8f}}};
            tipped(dl, c, r, bar, 4, true, ends, 2, fill, accent, alpha);
        }
        break;
    case 4: splitArrow(dl, c, r, 0, fill, alpha, accent); break;  // up: climb
    case 8: splitArrow(dl, c, r, 2, fill, alpha, accent); break;  // down
    case 9: splitArrow(dl, c, r, 3, fill, alpha, accent); break;  // left
    case 10: splitArrow(dl, c, r, 1, fill, alpha, accent); break; // right
    case 5: { // a flag on a pole
        const ImVec2 u[5] = {{-0.7f, -1.f}, {0.9f, -0.45f}, {-0.4f, 0.15f}, {-0.4f, 1.f}, {-0.7f, 1.f}};
        shape(dl, c, r, u, 5, fill, alpha, false);
        const ImVec2 inner[3] = {{-0.4f, -0.65f}, {0.25f, -0.43f}, {-0.4f, -0.13f}}; // the cloth at half size, against the pole
        shape(dl, c, r, inner, 3, accent, alpha, true);
        break;
    }
    case 6: { // a five-pointed star
        ImVec2 u[10], in[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -1.5707963f + static_cast<float>(i) * 0.6283185f; // 36 degrees
            const float k = (i & 1) ? 0.45f : 1.f;
            u[i] = ImVec2(std::cos(a) * k, std::sin(a) * k);
            in[i] = ImVec2(u[i].x * 0.5f, u[i].y * 0.5f);
        }
        shape(dl, c, r, u, 10, fill, alpha, false);
        shape(dl, c, r, in, 10, accent, alpha, false); // half size, in the accent
        break;
    }
    case 7: // a ring: here, at this spot
        ring(dl, c, 0.72f * r, std::max(2.f, 0.42f * r), fill, alpha);
        break;
    case 11: plus(dl, c, r, true, fill, accent, alpha); break;  // a cross: not here
    case 16: plus(dl, c, r, false, fill, accent, alpha); break; // a plus
    case 12: { // a check mark: its two ends, half a square each (the arm's width, cut to half), are the accent
        const ImVec2 u[6] = {{-0.95f, 0.f}, {-0.65f, -0.3f}, {-0.25f, 0.1f}, {0.65f, -0.8f}, {0.95f, -0.5f}, {-0.25f, 0.7f}};
        const ImVec2 ends[2][4] = {{{0.65f, -0.8f}, {0.95f, -0.5f}, {0.8015f, -0.3515f}, {0.5015f, -0.6515f}},
                                   {{-0.95f, 0.f}, {-0.65f, -0.3f}, {-0.5015f, -0.1515f}, {-0.8015f, 0.1485f}}};
        tipped(dl, c, r, u, 6, false, ends, 2, fill, accent, alpha);
        break;
    }
    case 13: { // a question mark on a disc: drawn from lines (a font drawn at offsets went soft), in the accent, edged dark
        disc(dl, c, 0.95f * r, fill, alpha);
        dl->AddCircle(c, 0.95f * r, dark(alpha), 0, 1.8f); // the disc's dark rim, a fifth thicker
        constexpr float kHookStart = 3.4906585f, kHookEnd = 7.8539816f; // 200 to 450 degrees: left, over the top, round, down
        const ImVec2 hook(c.x, c.y - 0.26f * r), stemEnd(c.x, c.y + 0.24f * r), dot(c.x, c.y + 0.58f * r);
        const float width = std::max(1.8f, 0.2f * r), dotR = std::max(1.4f, 0.11f * r);
        for (int pass = 0; pass < 2; ++pass) { // the dark edge first, a little wider, then the mark
            const float w = pass == 0 ? width + 3.f : width;
            const ImU32 color = pass == 0 ? ink : accent;
            dl->PathArcTo(hook, 0.36f * r, kHookStart, kHookEnd, 18);
            dl->PathLineTo(stemEnd);
            dl->PathStroke(color, ImDrawFlags_None, w);
            dl->AddCircleFilled(dot, pass == 0 ? dotR + 1.5f : dotR, color);
        }
        break;
    }
    case 33: { // an exclamation mark: the bar in the main color, the dot under it in the accent
        rect(dl, c, r, -0.2f, -0.95f, 0.2f, 0.3f, fill, alpha);
        disc(dl, ImVec2(c.x, c.y + 0.7f * r), 0.25f * r, accent, alpha);
        break;
    }
    case 14: { // a crosshair: an enemy; the four ticks are the accent
        const float w = std::max(1.5f, 0.2f * r);
        ring(dl, c, 0.6f * r, w, fill, alpha);
        const ImVec2 ticks[4] = {{0.f, -1.f}, {1.f, 0.f}, {0.f, 1.f}, {-1.f, 0.f}};
        for (const ImVec2& t : ticks) stroke(dl, c, r, ImVec2(t.x * 0.35f, t.y * 0.35f), t, w, accent, alpha);
        break;
    }
    case 15: { // a bolt (a shortcut): the game's two opposite half arrows, stood on end
        const ImVec2 u[5] = {{-1.f, -0.26f}, {-0.2f, -0.26f}, {-0.2f, -0.43f}, {0.51f, -0.03f}, {-0.69f, -0.03f}};
        ImVec2 v[5];
        for (int i = 0; i < 5; ++i) v[i] = ImVec2(-u[i].x, -u[i].y);
        turned(dl, c, r, u, 5, 1, fill, alpha, false);
        turned(dl, c, r, v, 5, 1, accent, alpha, false);
        break;
    }
    case 17: // no entry: a disc with a bar in the accent
        disc(dl, c, 0.95f * r, fill, alpha);
        rect(dl, c, r, -0.6f, -0.17f, 0.6f, 0.17f, accent, alpha);
        break;
    case 18: { // a heart, with the same one in the accent inside
        ImVec2 u[kMaxPoints];
        const int n = heartPoints(u);
        shape(dl, c, r, u, n, fill, alpha, false);
        ImVec2 in[kMaxPoints];
        shape(dl, c, r, in, heartInnerPoints(kHeartRim, in), accent, alpha, false, false);
        break;
    }
    case 19: { // the MagRope, as the game's gear icon draws it: a spike with a wing (the accent) swept back from its tip on each side
        const ImVec2 spike[4] = {{-0.04f, -0.77f}, {0.04f, -0.77f}, {0.46f, 0.79f}, {-0.46f, 0.79f}};
        shape(dl, c, r, spike, 4, fill, alpha, true);
        for (float side : {-1.f, 1.f}) {
            const ImVec2 wing[3] = {{0.18f * side, -0.74f}, {0.97f * side, -0.27f}, {0.59f * side, 0.12f}};
            shape(dl, c, r, wing, 3, accent, alpha, true);
        }
        break;
    }
    case 20: { // a minus: half a square at each end is the accent
        const ImVec2 bar[4] = {{-0.85f, -0.2f}, {0.85f, -0.2f}, {0.85f, 0.2f}, {-0.85f, 0.2f}};
        const ImVec2 ends[2][4] = {{{-0.85f, -0.2f}, {-0.65f, -0.2f}, {-0.65f, 0.2f}, {-0.85f, 0.2f}},
                                   {{0.65f, -0.2f}, {0.85f, -0.2f}, {0.85f, 0.2f}, {0.65f, 0.2f}}};
        tipped(dl, c, r, bar, 4, true, ends, 2, fill, accent, alpha);
        break;
    }
    case 21: // a target: a ring around a dot
        ring(dl, c, 0.78f * r, std::max(1.5f, 0.22f * r), fill, alpha);
        disc(dl, c, 0.375f * r, accent, alpha);
        break;
    case 22: { // look: the game's eye, a diamond frame around a solid diamond
        // The frame, as two halves. Smoothed edges would leave a seam where the halves meet; the
        // frame's own edges are drawn over below anyway. The frame grows outward (a quarter thicker);
        // its inner edge stays where it was.
        constexpr float kOutX = 1.0825f, kOutY = 0.6075f;
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (float side : {-1.f, 1.f}) {
            const ImVec2 half[6] = {{0.f, -kOutY}, {kOutX * side, 0.f}, {0.f, kOutY}, {0.f, 0.37f}, {0.67f * side, 0.f}, {0.f, -0.37f}};
            shape(dl, c, r, half, 6, fill, alpha, false, false);
        }
        dl->Flags = flags;
        const ImVec2 outer[4] = {{c.x, c.y - kOutY * r}, {c.x + kOutX * r, c.y}, {c.x, c.y + kOutY * r}, {c.x - kOutX * r, c.y}};
        dl->AddPolyline(outer, 4, dark(alpha), ImDrawFlags_Closed, 1.5f);
        const ImVec2 inner[4] = {{c.x, c.y - 0.375f * r}, {c.x + 0.67f * r, c.y}, {c.x, c.y + 0.375f * r}, {c.x - 0.67f * r, c.y}};
        dl->AddPolyline(inner, 4, dark(alpha), ImDrawFlags_Closed, 1.5f);
        const ImVec2 pupil[4] = {{0.f, -0.36f}, {0.33f, 0.f}, {0.f, 0.36f}, {-0.33f, 0.f}};
        shape(dl, c, r, pupil, 4, accent, alpha, true);
        break;
    }
    case 23: disc(dl, c, 0.6f * r, fill, alpha); break; // a dot
    case 24: { // a jump: the game's leaning arrow, split along its length
        const ImVec2 a[5] = {{0.66f, -0.97f}, {-0.36f, -0.32f}, {-0.15f, -0.15f}, {-0.76f, 1.f}, {-0.37f, 1.f}};
        const ImVec2 b[5] = {{0.76f, -0.97f}, {0.76f, 0.25f}, {0.53f, 0.22f}, {0.14f, 1.f}, {-0.27f, 1.f}};
        shape(dl, c, r, a, 5, fill, alpha, false);
        shape(dl, c, r, b, 5, accent, alpha, false);
        break;
    }
    case 25: { // a slide: the lower half of the arrow to the right (the accent), under a ceiling
        rect(dl, c, r, -1.f, -0.37f, 1.f, -0.07f, fill, alpha);
        const ImVec2 u[5] = {{1.f, 0.07f}, {-0.07f, 0.67f}, {-0.07f, 0.37f}, {-1.f, 0.37f}, {-1.f, 0.07f}};
        shape(dl, c, r, u, 5, accent, alpha, false);
        break;
    }
    case 26: { // a ladder or pipe climb: the game's two half arrows, one above the other (a fifth wider than the game's)
        constexpr float kWiden = 1.22f; // about x = -0.01: the height stays
        ImVec2 a[5] = {{0.31f, -0.98f}, {0.31f, 0.37f}, {0.12f, 0.12f}, {0.12f, -0.31f}, {-0.04f, -0.31f}};
        ImVec2 b[5] = {{0.02f, -0.27f}, {0.02f, 0.8f}, {-0.16f, 1.f}, {-0.16f, 0.39f}, {-0.33f, 0.39f}};
        for (ImVec2& p : a) p.x = -0.01f + (p.x + 0.01f) * kWiden;
        for (ImVec2& p : b) p.x = -0.01f + (p.x + 0.01f) * kWiden;
        shape(dl, c, r, a, 5, fill, alpha, false);
        shape(dl, c, r, b, 5, accent, alpha, false);
        break;
    }
    case 27: { // a zipline: a cable between two anchors (the accent)
        const ImVec2 top(-0.9f, -0.68f), low(0.9f, 0.48f);
        stroke(dl, c, r, top, low, std::max(1.5f, 0.16f * r), fill, alpha);
        disc(dl, ImVec2(c.x + top.x * r, c.y + top.y * r), 0.2f * r, accent, alpha);
        disc(dl, ImVec2(c.x + low.x * r, c.y + low.y * r), 0.2f * r, accent, alpha);
        break;
    }
    case 28: // a door, with a sign (the accent) on its upper half; no knob
        rect(dl, c, r, -0.6f, -0.95f, 0.6f, 0.95f, fill, alpha);
        rect(dl, c, r, -0.3f, -0.65f, 0.3f, -0.15f, accent, alpha); // 0.3 from the sides and from the top
        break;
    case 29: { // a wallrun: the game's wall with a run going up along it, twice (the smaller one is the accent)
        const ImVec2 a[6] = {{-0.73f, 1.f}, {-0.73f, -0.18f}, {0.73f, -1.f}, {0.73f, -0.46f}, {-0.23f, 0.03f}, {-0.23f, 1.f}};
        const ImVec2 b[6] = {{-0.09f, 1.f}, {-0.09f, 0.07f}, {0.73f, -0.39f}, {0.73f, -0.04f}, {0.23f, 0.21f}, {0.23f, 1.f}};
        shape(dl, c, r, a, 6, fill, alpha, false);
        shape(dl, c, r, b, 6, accent, alpha, false);
        break;
    }
    case 30: { // a shield, its top edge dipping in the middle (a 160 degree V), the same one in the accent inside
        const ImVec2 u[8] = {{-0.8f, -0.85f}, {0.f, -0.71f}, {0.8f, -0.85f}, {0.8f, 0.1f}, {0.45f, 0.65f}, {0.f, 1.f}, {-0.45f, 0.65f}, {-0.8f, 0.1f}};
        shape(dl, c, r, u, 8, fill, alpha, false);
        scaledAccent(dl, c, r, u, 8, ImVec2(0.f, 0.1f), kShieldScale, accent, alpha, false);
        break;
    }
    case 32: { // a vault: the upper half of the arrow to the right, over an obstacle (the slide, upside down)
        rect(dl, c, r, -1.f, 0.07f, 1.f, 0.37f, accent, alpha);
        const ImVec2 u[5] = {{1.f, -0.07f}, {-0.07f, -0.67f}, {-0.07f, -0.37f}, {-1.f, -0.37f}, {-1.f, -0.07f}};
        shape(dl, c, r, u, 5, fill, alpha, false);
        break;
    }
    case 31: { // a speech bubble: the bubble is the main color, the three dots the accent with a dark edge
        const ImVec2 u[7] = {{-0.95f, -0.8f}, {0.95f, -0.8f}, {0.95f, 0.35f}, {-0.1f, 0.35f}, {-0.55f, 0.95f}, {-0.5f, 0.35f}, {-0.95f, 0.35f}};
        shape(dl, c, r, u, 7, fill, alpha, false);
        const float dot = std::max(1.2f, 0.127f * r);
        for (float x : {-0.58f, 0.f, 0.58f}) {
            const ImVec2 at(c.x + x * r, c.y - 0.22f * r);
            dl->AddCircleFilled(at, dot + 1.5f, ink);
            dl->AddCircleFilled(at, dot, accent);
        }
        break;
    }
    default: { // 0: a diamond with a small diamond inside
        const ImVec2 u[4] = {{0.f, -1.f}, {0.7f, 0.f}, {0.f, 1.f}, {-0.7f, 0.f}};
        shape(dl, c, r, u, 4, fill, alpha, true);
        shape(dl, c, r * 0.45f, u, 4, accent, alpha, true);
        break;
    }
    }
}

} // namespace mecmp::ui
