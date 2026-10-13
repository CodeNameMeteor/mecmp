#pragma once
// In-game overlay: Dear ImGui drawn on top of the game.
//
// Hooks IDXGISwapChain::Present / ResizeBuffers by swapping their pointers in DXGI's swap-chain
// vtable (found through a throwaway device + swap chain), and subclasses the game window for
// input. While the menu is open, mouse and keyboard go to the menu instead of the game.

#include <string>

namespace mecmp::ui {

bool install();
// Called once on the window thread when the game window is destroyed (the game is exiting), while
// the process can still use the network. Set before install().
void setOnWindowDestroyed(void (*fn)());
bool menuOpen();
void toggleMenu(); // any thread (keyboard on the window thread, gamepad on the game's input thread)

// Keys that can open the menu. `name` as in mecmp.ini (INSERT, F5, ...).
struct MenuKey { const char* name; int vk; int imguiKey; };
const MenuKey* menuKeys(int& count);
void setMenuKey(const std::string& name);  // takes effect at once
void setKeyCapture(bool on);               // while the menu waits for a new key, the old one does not close it
// A key from mecmp.ini that is not the menu key (chat_key, ping_key): a letter A..Z, ENTER, or a menu
// key name. 0 for anything else (including empty = no key).
int actionKeyFromName(const std::string& name);

} // namespace mecmp::ui
