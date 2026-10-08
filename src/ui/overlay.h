#pragma once
// In-game overlay: Dear ImGui drawn on top of the game.
//
// Hooks IDXGISwapChain::Present / ResizeBuffers by swapping their pointers in DXGI's swap-chain
// vtable (found through a throwaway device + swap chain), and subclasses the game window for
// input. While the menu is open, mouse and keyboard go to the menu instead of the game.

namespace mecmp::ui {

bool install();
bool menuOpen();

} // namespace mecmp::ui
