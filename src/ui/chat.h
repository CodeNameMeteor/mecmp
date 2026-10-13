#pragma once
// Text chat: the last messages bottom-left (fading out after a while), and a line to type into.
// The overlay opens the input with the chat key (mecmp.ini [ui] chat_key, ENTER by default) and, while it
// is open, gives the keyboard to it instead of the game. Enter sends, Escape cancels.

namespace mecmp::ui::chat {

void open();
void close();
bool isOpen();
void draw(); // render thread, inside the ImGui frame

} // namespace mecmp::ui::chat
