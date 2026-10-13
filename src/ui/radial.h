#pragma once
// The marker wheel. A short press of the marker key (or gamepad button) places a marker; holding it
// for kHoldMs opens a wheel of nine marker types around the middle of the screen. Point at one with
// the mouse or the right stick and let go (or click) to place it; letting go in the middle cancels.
// There are several wheels: the mouse wheel, the Left/Right arrows or LB/RB switch between them. While
// the key is down, the digits 1..9 place that slot of the current wheel at once.
//
// Removing all your markers: hold the "clear markers" key for kHoldMs anywhere in the game, or the
// middle mouse button while the wheel is open; on a gamepad, press the "clear markers" buttons while
// the marker buttons are held.
//
// While the wheel is open the mouse (or the right stick) is the wheel's, not the game's, so the camera
// stays still; the keyboard still reaches the game, so Faith keeps running. The wheel itself is drawn
// by the overlay.

namespace mecmp::ui::radial {

constexpr unsigned kHoldMs = 250;

bool open();    // the wheel is shown (any thread)
bool engaged(); // the marker key is down and not used up yet: digits pick a slot (any thread)

// Window thread (the overlay's window procedure):
void keyDown(int key);          // the marker key `key` went down (not a repeat)
void keyUp();                   // it was released
void digit(int n);              // 1..9 was pressed while engaged
void moved(float dx, float dy); // raw mouse movement while open (pixels)
void click();                   // left click while open: place what is pointed at and close
void switchWheel(int step);     // +1 / -1 while open
void cancel();                  // lost focus, or the menu or the chat opened: close, place nothing

// Game's input thread, for the pad whose marker buttons are (or just were) held, on every read.
// `pressed` = the buttons went down on this read; x, y = the right stick, -1..1 with y down;
// prev / next = LB / RB went down; clear = the "clear markers" buttons went down. True while the wheel
// is open: the stick and LB/RB are the wheel's.
bool padUpdate(bool pressed, bool held, float x, float y, bool prev, bool next, bool clear);

// Render thread, every frame (after NewFrame): opens the wheel when the key was held long enough,
// draws it, and closes it if the key is no longer held. Also watches the "clear markers" key.
void draw();

} // namespace mecmp::ui::radial
