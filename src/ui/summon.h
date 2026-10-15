#pragma once
// Summon, the player's side ("come to me"; packets in net/protocol.h, rules in net/summon_policy.h).
//
//   - calling: a tap on summon_key calls everyone, holding it for a second pulls them in (hard), holding
//     it while the call is open withdraws it (the menu's Players tab can call chosen players too);
//   - being called: a panel at the top says who calls, a marker ("Summon point") shows where, and
//     holding the accept key for a second goes there, holding the decline key says no. A hard summon
//     counts down first and the player may cancel it in that time;
//   - after being taken, "Go back?" stays for a minute: holding the decline key for 3 seconds takes the
//     player back to where they stood.
//
// Everything runs on the render thread except takesAcceptKey / takesDeclineKey.

#include <cstdint>
#include <vector>

namespace mecmp::ui::summon {

// What the keys and buttons are doing this frame (the overlay reads them: keyboard, mouse side buttons,
// gamepad). `canAct` = the game has the keyboard and nothing else (menu, chat, a game menu, the marker
// wheel) is using it.
struct Input {
    bool canAct = false;
    bool sendDown = false;
    bool acceptDown = false;
    bool declineDown = false;
};
void update(const Input& in); // once a frame, before draw()
// The level changed: the summons waiting from the old one are dropped (a hard one would otherwise be taken
// in the new level). Any thread; the render thread does the clearing in update().
void levelChanged();
void draw();                  // the panel and the markers

// True while a key is taken from the game: the accept key while a summon waits, the decline key also
// while "Go back?" is shown. Any thread (the window thread and the pad hook ask).
bool takesAcceptKey();
bool takesDeclineKey();

// Calls `targets` (session ids; empty = everyone) to where we stand, hard or not. Says what happened
// in a notice. False when nothing was sent.
bool send(const std::vector<uint32_t>& targets, bool hard);

} // namespace mecmp::ui::summon
