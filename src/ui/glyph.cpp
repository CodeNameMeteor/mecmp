#include "ui/glyph.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace mecmp::ui {
namespace {

constexpr int kMaxPoints = 20;
constexpr float kPi = 3.14159265f;

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
// clockwise give right, down and left.
void splitArrow(ImDrawList* dl, ImVec2 c, float r, int turns, ImU32 fill, float alpha) {
    for (float side : {-1.f, 1.f}) {
        const ImVec2 u[5] = {{0.07f * side, -1.f}, {0.67f * side, 0.07f}, {0.37f * side, 0.07f}, {0.37f * side, 1.f}, {0.07f * side, 1.f}};
        turned(dl, c, r, u, 5, turns, fill, alpha, false);
    }
}

// A plus sign with arms of half-width w; turned by 45 degrees it is the cross.
void plus(ImDrawList* dl, ImVec2 c, float r, bool diagonal, ImU32 fill, float alpha) {
    const float w = 0.26f, l = diagonal ? 1.05f : 0.9f;
    ImVec2 u[12] = {{-w, -l}, {w, -l}, {w, -w}, {l, -w}, {l, w}, {w, w}, {w, l}, {-w, l}, {-w, w}, {-l, w}, {-l, -w}, {-w, -w}};
    if (diagonal)
        for (ImVec2& p : u) p = ImVec2((p.x - p.y) * 0.70710678f, (p.x + p.y) * 0.70710678f);
    shape(dl, c, r, u, 12, fill, alpha, false);
}
} // namespace

void drawGlyph(ImDrawList* dl, uint8_t glyph, ImVec2 c, float r, ImU32 fill, float alpha, ImU32 accent) {
    const ImU32 ink = dark(alpha, 235);
    switch (glyph) {
    case 1: { // chevron pointing down: "go there"
        const ImVec2 u[6] = {{-0.9f, -0.75f}, {-0.9f, -0.15f}, {0.f, 0.75f}, {0.9f, -0.15f}, {0.9f, -0.75f}, {0.f, 0.15f}};
        shape(dl, c, r, u, 6, fill, alpha, false);
        break;
    }
    case 2: { // danger: a triangle with an exclamation mark
        const ImVec2 u[3] = {{0.f, -1.f}, {0.95f, 0.8f}, {-0.95f, 0.8f}};
        shape(dl, c, r, u, 3, fill, alpha, true);
        dl->AddLine(ImVec2(c.x, c.y - 0.3f * r), ImVec2(c.x, c.y + 0.22f * r), ink, std::max(2.f, 0.22f * r));
        dl->AddCircleFilled(ImVec2(c.x, c.y + 0.52f * r), std::max(1.5f, 0.12f * r), ink);
        break;
    }
    case 3: // wait: two bars
        rect(dl, c, r, -0.62f, -0.8f, -0.18f, 0.8f, fill, alpha);
        rect(dl, c, r, 0.18f, -0.8f, 0.62f, 0.8f, fill, alpha);
        break;
    case 4: splitArrow(dl, c, r, 0, fill, alpha); break;  // up: climb
    case 8: splitArrow(dl, c, r, 2, fill, alpha); break;  // down
    case 9: splitArrow(dl, c, r, 3, fill, alpha); break;  // left
    case 10: splitArrow(dl, c, r, 1, fill, alpha); break; // right
    case 5: { // a flag on a pole
        const ImVec2 u[5] = {{-0.7f, -1.f}, {0.9f, -0.45f}, {-0.4f, 0.15f}, {-0.4f, 1.f}, {-0.7f, 1.f}};
        shape(dl, c, r, u, 5, fill, alpha, false);
        const ImVec2 inner[3] = {{-0.4f, -0.65f}, {0.25f, -0.43f}, {-0.4f, -0.13f}}; // the cloth at half size, against the pole
        shape(dl, c, r, inner, 3, accent, alpha, true);
        break;
    }
    case 6: { // a five-pointed star
        ImVec2 u[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -1.5707963f + static_cast<float>(i) * 0.6283185f; // 36 degrees
            const float k = (i & 1) ? 0.45f : 1.f;
            u[i] = ImVec2(std::cos(a) * k, std::sin(a) * k);
        }
        shape(dl, c, r, u, 10, fill, alpha, false);
        break;
    }
    case 7: // a ring: here, at this spot
        ring(dl, c, 0.72f * r, std::max(2.f, 0.42f * r), fill, alpha);
        break;
    case 11: plus(dl, c, r, true, fill, alpha); break;  // a cross: not here
    case 16: plus(dl, c, r, false, fill, alpha); break; // a plus
    case 12: { // a check mark
        const ImVec2 u[6] = {{-0.95f, 0.f}, {-0.65f, -0.3f}, {-0.25f, 0.1f}, {0.65f, -0.8f}, {0.95f, -0.5f}, {-0.25f, 0.7f}};
        shape(dl, c, r, u, 6, fill, alpha, false);
        break;
    }
    case 13: { // a question mark on a disc
        disc(dl, c, 0.95f * r, fill, alpha);
        ImFont* font = ImGui::GetFont();
        const float size = 1.5f * r;
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, "?");
        dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), ink, "?");
        break;
    }
    case 14: { // a crosshair: an enemy
        const float w = std::max(1.5f, 0.2f * r);
        ring(dl, c, 0.6f * r, w, fill, alpha);
        const ImVec2 ticks[4] = {{0.f, -1.f}, {1.f, 0.f}, {0.f, 1.f}, {-1.f, 0.f}};
        for (const ImVec2& t : ticks) stroke(dl, c, r, ImVec2(t.x * 0.35f, t.y * 0.35f), t, w, fill, alpha);
        break;
    }
    case 15: { // a bolt (a shortcut): the game's two opposite half arrows, stood on end
        const ImVec2 u[5] = {{-1.f, -0.26f}, {-0.2f, -0.26f}, {-0.2f, -0.43f}, {0.51f, -0.03f}, {-0.69f, -0.03f}};
        ImVec2 v[5];
        for (int i = 0; i < 5; ++i) v[i] = ImVec2(-u[i].x, -u[i].y);
        turned(dl, c, r, u, 5, 1, fill, alpha, false);
        turned(dl, c, r, v, 5, 1, fill, alpha, false);
        break;
    }
    case 17: // no entry: a disc with a bar
        disc(dl, c, 0.95f * r, fill, alpha);
        dl->AddRectFilled(ImVec2(c.x - 0.6f * r, c.y - 0.17f * r), ImVec2(c.x + 0.6f * r, c.y + 0.17f * r), ink);
        break;
    case 18: { // a heart
        ImVec2 u[kMaxPoints];
        for (int i = 0; i < kMaxPoints; ++i) {
            const float t = 2.f * kPi * static_cast<float>(i) / kMaxPoints;
            const float s = std::sin(t);
            u[i] = ImVec2(16.f * s * s * s / 17.f,
                          -(13.f * std::cos(t) - 5.f * std::cos(2.f * t) - 2.f * std::cos(3.f * t) - std::cos(4.f * t)) / 17.f - 0.12f);
        }
        shape(dl, c, r, u, kMaxPoints, fill, alpha, false);
        break;
    }
    case 19: { // the MagRope, as the game's gear icon draws it: a spike with a wing swept back from its tip on each side
        const ImVec2 spike[4] = {{-0.04f, -0.77f}, {0.04f, -0.77f}, {0.46f, 0.79f}, {-0.46f, 0.79f}};
        shape(dl, c, r, spike, 4, fill, alpha, true);
        for (float side : {-1.f, 1.f}) {
            const ImVec2 wing[3] = {{0.18f * side, -0.74f}, {0.97f * side, -0.27f}, {0.59f * side, 0.12f}};
            shape(dl, c, r, wing, 3, fill, alpha, true);
        }
        break;
    }
    case 20: rect(dl, c, r, -0.85f, -0.2f, 0.85f, 0.2f, fill, alpha); break; // a minus
    case 21: // a target: a ring around a dot
        ring(dl, c, 0.78f * r, std::max(1.5f, 0.22f * r), fill, alpha);
        disc(dl, c, 0.3f * r, accent, alpha);
        break;
    case 22: { // look: the game's eye, a diamond frame around a solid diamond
        // The frame, as two halves. Smoothed edges would leave a seam where the halves meet; the
        // frame's own edges are drawn over below anyway.
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (float side : {-1.f, 1.f}) {
            const ImVec2 half[6] = {{0.f, -0.56f}, {side, 0.f}, {0.f, 0.56f}, {0.f, 0.37f}, {0.67f * side, 0.f}, {0.f, -0.37f}};
            shape(dl, c, r, half, 6, fill, alpha, false, false);
        }
        dl->Flags = flags;
        for (float k : {1.f, 0.67f}) { // its outer and inner edge
            const ImVec2 edge[4] = {{c.x, c.y - 0.56f * k * r}, {c.x + k * r, c.y}, {c.x, c.y + 0.56f * k * r}, {c.x - k * r, c.y}};
            dl->AddPolyline(edge, 4, dark(alpha), ImDrawFlags_Closed, 1.5f);
        }
        const ImVec2 pupil[4] = {{0.f, -0.36f}, {0.33f, 0.f}, {0.f, 0.36f}, {-0.33f, 0.f}};
        shape(dl, c, r, pupil, 4, accent, alpha, true);
        break;
    }
    case 23: disc(dl, c, 0.6f * r, fill, alpha); break; // a dot
    case 24: { // a jump: the game's leaning arrow, split along its length
        const ImVec2 a[5] = {{0.66f, -0.97f}, {-0.36f, -0.32f}, {-0.15f, -0.15f}, {-0.76f, 1.f}, {-0.37f, 1.f}};
        const ImVec2 b[5] = {{0.76f, -0.97f}, {0.76f, 0.25f}, {0.53f, 0.22f}, {0.14f, 1.f}, {-0.27f, 1.f}};
        shape(dl, c, r, a, 5, fill, alpha, false);
        shape(dl, c, r, b, 5, fill, alpha, false);
        break;
    }
    case 25: { // a slide: the lower half of the arrow to the right, under a ceiling
        rect(dl, c, r, -1.f, -0.37f, 1.f, -0.07f, fill, alpha);
        const ImVec2 u[5] = {{1.f, 0.07f}, {-0.07f, 0.67f}, {-0.07f, 0.37f}, {-1.f, 0.37f}, {-1.f, 0.07f}};
        shape(dl, c, r, u, 5, fill, alpha, false);
        break;
    }
    case 26: { // a ladder or pipe climb: the game's two half arrows, one above the other
        const ImVec2 a[5] = {{0.31f, -0.98f}, {0.31f, 0.37f}, {0.12f, 0.12f}, {0.12f, -0.31f}, {-0.04f, -0.31f}};
        const ImVec2 b[5] = {{0.02f, -0.27f}, {0.02f, 0.8f}, {-0.16f, 1.f}, {-0.16f, 0.39f}, {-0.33f, 0.39f}};
        shape(dl, c, r, a, 5, fill, alpha, false);
        shape(dl, c, r, b, 5, fill, alpha, false);
        break;
    }
    case 27: { // a zipline: a cable between two anchors
        const ImVec2 top(-0.9f, -0.68f), low(0.9f, 0.48f);
        stroke(dl, c, r, top, low, std::max(1.5f, 0.16f * r), fill, alpha);
        disc(dl, ImVec2(c.x + top.x * r, c.y + top.y * r), 0.2f * r, fill, alpha);
        disc(dl, ImVec2(c.x + low.x * r, c.y + low.y * r), 0.2f * r, fill, alpha);
        break;
    }
    case 28: // a door
        rect(dl, c, r, -0.6f, -0.95f, 0.6f, 0.95f, fill, alpha);
        dl->AddCircleFilled(ImVec2(c.x + 0.3f * r, c.y + 0.05f * r), std::max(1.5f, 0.13f * r), ink);
        break;
    case 29: { // a wallrun: the game's wall with a run going up along it, twice
        const ImVec2 a[6] = {{-0.73f, 1.f}, {-0.73f, -0.18f}, {0.73f, -1.f}, {0.73f, -0.46f}, {-0.23f, 0.03f}, {-0.23f, 1.f}};
        const ImVec2 b[6] = {{-0.09f, 1.f}, {-0.09f, 0.07f}, {0.73f, -0.39f}, {0.73f, -0.04f}, {0.23f, 0.21f}, {0.23f, 1.f}};
        shape(dl, c, r, a, 6, fill, alpha, false);
        shape(dl, c, r, b, 6, fill, alpha, false);
        break;
    }
    case 30: { // a shield
        const ImVec2 u[7] = {{-0.8f, -0.85f}, {0.8f, -0.85f}, {0.8f, 0.1f}, {0.45f, 0.65f}, {0.f, 1.f}, {-0.45f, 0.65f}, {-0.8f, 0.1f}};
        shape(dl, c, r, u, 7, fill, alpha, false);
        break;
    }
    case 32: { // a vault: the upper half of the arrow to the right, over an obstacle (the slide, upside down)
        rect(dl, c, r, -1.f, 0.07f, 1.f, 0.37f, fill, alpha);
        const ImVec2 u[5] = {{1.f, -0.07f}, {-0.07f, -0.67f}, {-0.07f, -0.37f}, {-1.f, -0.37f}, {-1.f, -0.07f}};
        shape(dl, c, r, u, 5, fill, alpha, false);
        break;
    }
    case 31: { // a speech bubble
        const ImVec2 u[7] = {{-0.95f, -0.8f}, {0.95f, -0.8f}, {0.95f, 0.35f}, {-0.1f, 0.35f}, {-0.55f, 0.95f}, {-0.5f, 0.35f}, {-0.95f, 0.35f}};
        shape(dl, c, r, u, 7, fill, alpha, false);
        for (float x : {-0.45f, 0.f, 0.45f}) dl->AddCircleFilled(ImVec2(c.x + x * r, c.y - 0.22f * r), std::max(1.f, 0.11f * r), ink);
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
