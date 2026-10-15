#pragma once
// Summon: the decisions that need no game and no sockets, so tests can check them (see SUMMON in
// protocol.h for the packets).
//
// Who may pull a player in:
//   - a plain ("soft") summon: anyone; the receiver is asked and answers with a hold of a key;
//   - a hard summon: only what the receiver trusts (the lobby host, or a member the host vouched for),
//     and only when the receiver opted in. Anything else turns into a soft summon, so a hard one never
//     takes control away from a player who did not allow it.

#include <cstdint>
#include <string>

namespace mecmp::net::summon {

constexpr uint32_t kMinGapMs = 5000;        // one sender may start a summon this often at most
constexpr uint32_t kSendCooldownMs = 5000; // we start one this often at most (not below kMinGapMs)
constexpr uint32_t kSoftWindowMs = 60000;   // a summon stays open for the receiver this long
constexpr uint32_t kReportWindowMs = 65000; // the sender waits for answers this long (a little over)

enum class Action {
    Ignore, // not for us, or too soon after the last one from the same player
    Soft,   // ask the player
    Hard,   // count down and go, the player may cancel in time
};

struct Input {
    bool fromSelf = false;         // our own packet
    bool calledMe = false;         // everyone, or our session id is in the list
    bool hardRequested = false;    // the sender asked for a hard summon
    bool senderIsHost = false;     // the sender is the player hosting this lobby (not a dedicated server)
    bool vouched = false;          // the packet carries the host's vouch...
    bool vouchTrusted = false;     // ...and it came by a path where only a player-host sets it
    bool optedIn = false;          // the receiving player allows hard summons (a setting)
    uint32_t sinceLastFromSenderMs = 0xffffffffu; // since the last summon this sender started
};

inline Action decide(const Input& in) {
    if (in.fromSelf || !in.calledMe) return Action::Ignore;
    if (in.sinceLastFromSenderMs < kMinGapMs) return Action::Ignore;
    const bool trusted = in.senderIsHost || (in.vouched && in.vouchTrusted);
    if (in.hardRequested && trusted && in.optedIn) return Action::Hard;
    return Action::Soft;
}

// What the caller is told once the answers are in (or the time ran out). One called player gets its
// own sentence; several get a count.
struct Tally {
    int expected = 0;     // how many were called
    int teleporting = 0;  // answered "on my way"
    int declined = 0;
    int busy = 0;         // could not go right now
    std::string singleName; // the one player called (used when expected == 1)
};

inline int answered(const Tally& t) { return t.teleporting + t.declined + t.busy; }

inline bool complete(const Tally& t) { return t.expected > 0 && answered(t) >= t.expected; }

inline std::string reportText(const Tally& t) {
    if (t.expected <= 1) {
        const std::string who = t.singleName.empty() ? std::string("Runner") : t.singleName;
        if (t.teleporting) return who + " accepted your summon";
        if (t.busy) return who + " can't come right now";
        if (t.declined) return who + " declined your summon";
        return who + " didn't answer";
    }
    // "of M" is how many were called; a player who joined meanwhile and answered counts too.
    const int total = t.expected > answered(t) ? t.expected : answered(t);
    return "Summon: " + std::to_string(t.teleporting) + " of " + std::to_string(total) + " teleported";
}

} // namespace mecmp::net::summon
