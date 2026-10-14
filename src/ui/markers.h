#pragma once
// World markers placed by players ("pings") and arrows on the screen's edge towards markers and
// players that are off screen. Render thread, inside the ImGui frame.

#include <cstdint>
#include <string>

namespace mecmp::ui::markers {

void draw();

// The marker the crosshair is nearest to on the last drawn frame (within a few markers' widths; markers
// that are off screen do not count). It is drawn at full size, as the one a double tap removes (own) or
// a quick answer goes to (other). Any thread.
struct Focus {
    bool valid = false;
    uint32_t owner = 0; // session id of the player who placed it
    uint8_t index = 0;  // which of the owner's cells it is in
    std::string name;   // the owner's name
    std::string label;  // the marker's title; may be empty
};
Focus focusedOwn();   // one of mine
Focus focusedOther(); // another player's

} // namespace mecmp::ui::markers
