#pragma once
// The marker wheel. A short press of the marker key (or gamepad button) places a marker; holding it
// for kHoldMs opens a wheel of up to nine marker types around the middle of the screen (with
// ping_wheel_mode=instant the wheel opens the moment the key goes down, and only a very short tap
// that never moved the pointer still places a marker). Point at one with the mouse or the right
// stick and let go (or click) to place it; letting go in the middle, or the right mouse button,
// cancels. There are several wheels: the mouse wheel, the Left/Right arrows or LB/RB switch between
// them. While the key is down, the digits 1..9 place that slot of the current wheel at once.
//
// The same wheel, filled with short answers, is the quick answers wheel: its own key (react_key)
// sends the answer to the owner of the marker you are looking at (see Mode::React).
//
// Removing markers: tap the "clear markers" key twice to remove the one you look at (at your feet in
// "where I stand" mode); hold it for kHoldMs anywhere in the game to remove all of them, or the
// middle mouse button while the wheel is open; on a gamepad, press the "clear markers" buttons while
// the marker buttons are held.
//
// While the wheel is open the mouse (or the right stick) is the wheel's, not the game's, so the camera
// stays still; the keyboard still reaches the game, so Faith keeps running. The wheel itself is drawn
// by the overlay.

namespace mecmp::ui::radial {

constexpr unsigned kHoldMs = 250;

// What the wheel is for: placing markers, or answering the marker the player looks at.
enum class Mode { Markers, React };

bool open();    // the wheel is shown (any thread)
bool engaged(); // the marker key is down and not used up yet: digits pick a slot (any thread)

// Window thread (the overlay's window procedure):
void keyDown(int key, Mode mode = Mode::Markers); // the key `key` went down (not a repeat)
void keyUp(int key);           // the key `key` was released: ends the wheel that key opened, no other
void digit(int n);              // 1..9 was pressed while engaged
void moved(float dx, float dy); // raw mouse movement while open (pixels)
void click();                   // left click while open: place what is pointed at and close
void dismiss();                 // right click while open: close, place nothing
void switchWheel(int step);     // +1 / -1 while open
void cancel();                  // lost focus, or the menu or the chat opened: close, place nothing

// Game's input thread, for the pad whose buttons are (or just were) held, on every read. `mode` is
// what the held buttons are for. `pressed` = the buttons went down on this read; x, y = the right
// stick, -1..1 with y down; prev / next = LB / RB went down; clear = the "clear markers" buttons went
// down. True while the wheel is open: the stick and LB/RB are the wheel's.
bool padUpdate(Mode mode, bool pressed, bool held, float x, float y, bool prev, bool next, bool clear);

// Render thread, every frame (after NewFrame): opens the wheel when the key was held long enough,
// draws it, and closes it if the key is no longer held. Also watches the "clear markers" key.
void draw();

} // namespace mecmp::ui::radial
