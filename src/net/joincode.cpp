#include "net/joincode.h"

#include <cctype>

namespace mecmp::net::joincode {
namespace {
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int value(char c) {
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}
} // namespace

std::string encode(uint32_t ip, uint16_t port) {
    const uint64_t v = (static_cast<uint64_t>(ip) << 16) | port; // 48 bits -> 10 symbols
    std::string s;
    for (int i = 9; i >= 0; --i) {
        s += kAlphabet[(v >> (i * 5)) & 31];
        if (i == 5) s += '-';
    }
    return s;
}

bool decode(const std::string& code, uint32_t& ip, uint16_t& port) {
    uint64_t v = 0;
    int n = 0;
    for (char c : code) {
        if (c == '-' || c == ' ') continue;
        const int d = value(c);
        if (d < 0) return false;
        v = (v << 5) | static_cast<uint64_t>(d);
        ++n;
    }
    if (n != 10) return false;
    // encode() fills 48 bits; a first symbol of 8 or more would set bits that the address does not have.
    if (v >> 48) return false;
    ip = static_cast<uint32_t>(v >> 16);
    port = static_cast<uint16_t>(v & 0xffff);
    // Only unicast addresses can be a host: no 0.0.0.0, broadcast, multicast (224.0.0.0/4) or reserved (240.0.0.0/4).
    if (ip == 0 || ip == 0xffffffffu || (ip >> 28) >= 0xe) return false;
    return port != 0;
}

} // namespace mecmp::net::joincode
