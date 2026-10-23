#pragma once
// Short notices at the top of the screen: a player joins or leaves, or places a marker (render
// thread, inside the ImGui frame). Session events distinguish exits from connection timeouts.

#include <string>

namespace mecmp::ui::notices {

void draw();
// Shows `text` for a few seconds. Render thread only (it is called from draw() or other overlay code).
void push(std::string text);

} // namespace mecmp::ui::notices
