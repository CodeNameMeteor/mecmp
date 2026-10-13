#pragma once
// The menu's server list: asks each saved server "who are you?" (an INFO packet, protocol.h) from its
// own small socket and thread, and keeps the answers: name, players, round trip, version. Names are
// looked up on that thread, so a slow DNS answer never stalls the game. Nothing is sent while the
// menu is closed.

#include <cstdint>
#include <string>
#include <vector>

namespace mecmp::net::servers {

struct Entry {
    std::string address;     // as saved ("play.example.com", "1.2.3.4:47800")
    bool resolved = false;   // the address could be looked up
    bool answered = false;   // an answer arrived in the last 15 s
    std::string name;        // the server's name (last answer)
    int players = 0, maxPlayers = 0;
    int pingMs = -1;
    uint16_t protocol = 0;   // the server's protocol (another version: cannot join)
};

// Menu thread. The list to ask (the saved servers); `active` while the menu is open.
void update(const std::vector<std::string>& addresses, bool active);
std::vector<Entry> snapshot();
void refreshNow();
void shutdown(); // game exit

// "a, b ,c" <-> {"a","b","c"} (mecmp.ini [net] servers=)
std::vector<std::string> split(const std::string& list);
std::string join(const std::vector<std::string>& addresses);

} // namespace mecmp::net::servers
