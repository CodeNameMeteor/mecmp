#pragma once
// Join codes: a host's public IPv4 address and UDP port as 10 characters, e.g. "8H3KQ-ZT1V4".
// Crockford base32 (no I, L, O, U), so codes are easy to read out; dashes and case don't matter.
// Plain "ip:port" or "host:port" is accepted wherever a code is.

#include <cstdint>
#include <string>

namespace mecmp::net::joincode {

std::string encode(uint32_t ipv4HostOrder, uint16_t port);
bool decode(const std::string& code, uint32_t& ipv4HostOrder, uint16_t& port);

} // namespace mecmp::net::joincode
