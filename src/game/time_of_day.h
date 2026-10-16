#pragma once
// Time of day: everyone in a session sees the host's sky.
//
// The city's clock is a TimeOfDayClockEntity. Its constructor (Ghidra: TimeOfDayClockEntity_ctor, 0x143bc3be0)
// makes one "TimeOfDayClockUpdater" per realm and keeps it in a static array (g_timeOfDayClockUpdater,
// 0x142598ac0, [realm]); the updater (vtable 0x141cd82a0) holds the clock at +0x28, and the clock holds the time
// at +0x30: a float, seconds since midnight (0..86400). (Found as the Cheat Engine path
// [[0x14255c2f8]+0x28]+0x30: 0x14255c2f8 is the NetworkedMessageUpdater's "next updater" field, which in this
// build is the TimeOfDayClockUpdater; going through the static array does not depend on that order.)
//
// A host sends its time with its HELLO (net/protocol.h); a joiner whose host is a player writes it into its own
// clock when the two are more than a few seconds apart ([player] sync_time_of_day). A data write, like the
// ghosts' poses: no game code is changed. Every write is checked first (the updater's vtable, a sane time).

namespace mecmp::game::time_of_day {

// Server game thread, every tick (it does its work about once a second).
void tick();
// The local clock now (seconds since midnight), or false if there is none (main menu, loading, another build).
bool current(float& seconds);
// For the menu: what the sync is doing ("following the host", "you host: the others follow you", ...).
const char* status();

} // namespace mecmp::game::time_of_day
