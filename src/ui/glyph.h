#pragma once
// The icons a world marker can have, drawn with ImGui primitives. Shared by the markers in the world,
// the marker wheel and the menu, so an icon looks the same everywhere.

#include <cstdint>

#include "core/config.h"
#include "imgui.h"

namespace mecmp::ui {

constexpr int kGlyphCount = config::kPingGlyphs;

// The ring (7) and the dot (23) are no longer offered in the icon list. Their numbers stay: markers
// from older versions and mecmp.ini files may carry them, and they are still drawn.
inline bool glyphPickable(int glyph) { return glyph != 7 && glyph != 23; }

// Icon `glyph` (see config::glyphName for the list; anything unknown draws as 0) centred at `c`, about
// 2*r tall, filled with `fill` and outlined dark. `alpha` (0..1) fades the outline and the details
// drawn over the fill. `accent` colours one part of almost every icon (a half of an arrow, the detail
// inside the shape, the exclamation mark ...).
void drawGlyph(ImDrawList* dl, uint8_t glyph, ImVec2 c, float r, ImU32 fill, float alpha,
               ImU32 accent = IM_COL32(255, 255, 255, 255));

} // namespace mecmp::ui
