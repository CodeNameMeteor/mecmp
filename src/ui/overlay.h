#pragma once
// In-game overlay: Dear ImGui drawn on top of the game.
//
// Hooks IDXGISwapChain::Present / ResizeBuffers by swapping their pointers in DXGI's swap-chain
// vtable (found through a throwaway device + swap chain), and subclasses the game window for
// input. While the menu is open, mouse and keyboard go to the menu instead of the game.

namespace mecmp::ui {

bool install();
// Called once on the window thread when the game window is destroyed (the game is exiting), while
// the process can still use the network. Set before install().
void setOnWindowDestroyed(void (*fn)());
bool menuOpen();

} // namespace mecmp::ui
