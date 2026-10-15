#pragma once
// Gamepad: opening the menu (and other mod actions) with a button combination, and keeping the game
// from seeing the pad while the menu is open.
//
// The game loads XInput itself and keeps XInputGetState in a data pointer (0x14256d7e0, written
// once by 0x1437ba6c0). The mod swaps that pointer for its own function, which calls the real one:
//   - it sees every button press, so the menu combination works in game;
//   - while the menu is open (and until the combination is released) it hands the game a neutral
//     pad, so menu navigation does not move Faith.
// The game's controller-detection pointer (0x142607db8) is left alone, so the pad never looks
// unplugged. The menu itself reads the pad through Dear ImGui's own XInput access.

#include <cstdint>
#include <string>

namespace mecmp::ui::gamepad {

// Mod thread, regularly: installs the pointer swap once the game has loaded XInput.
void maintain();

// Button sets use XINPUT_GAMEPAD_* bits. Names: A B X Y LB RB LS RS BACK START UP DOWN LEFT RIGHT.
uint16_t parse(const std::string& combo); // "LS+RS" -> bits (0 for empty or unknown)
std::string format(uint16_t buttons);     // bits -> "LS+RS"
// What a combination does. Menu opens/closes the menu; Ping places a marker on a short press and
// opens the marker wheel when held (ui/radial.h; not while the menu is open). The combination's
// buttons are hidden from the game until they are released; while the marker wheel is open, so are
// the right stick and LB/RB.
// PingClear removes all your markers; it only acts on the pad whose Ping buttons are held.
// React is the quick answers wheel: a short press sends the last answer to the marker you look at,
// holding opens the answers wheel (the same stick and cancel rules as Ping).
// SummonAccept / SummonDecline are held (not pressed) buttons that answer a summon (ui/summon.h);
// they are read with held() and hidden from the game only while setSummonHidden asks for it.
enum class Action { Menu, Ping, PingClear, React, SummonAccept, SummonDecline, Count };
void setBinding(uint16_t buttons, Action action = Action::Menu); // 0 = none
// False if another action already has exactly these buttons (a press would do two things at once).
bool bindingFree(uint16_t buttons, Action action);
// True while all of the action's buttons are held on some pad (never while a binding is recorded).
bool held(Action action);
// The summon panel is up: the game does not see the accept / decline buttons (as for the open wheel).
void setSummonHidden(bool accept, bool decline);

// A button as the player's pad names it in hints: the Xbox name, or the PlayStation one when
// [ui] pad_labels=playstation (A = Cross, B = Circle, X = Square, Y = Triangle, LB/RB = L1/R1,
// LS/RS = L3/R3, BACK = Share, START = Options). The settings themselves always hold Xbox names.
std::string label(const std::string& button);
std::string labelCombo(const std::string& combo); // "LS+RS" -> "L3+R3"; "none" for an empty one

// Binding from the menu: start, then poll until it returns true (the buttons held together,
// reported once all are released). cancelCapture() stops it.
void beginCapture();
bool captureDone(uint16_t& buttons);
void cancelCapture();
bool capturing();

bool active(); // the game's pad reads go through the mod (some pad was seen)

} // namespace mecmp::ui::gamepad
