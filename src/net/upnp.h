#pragma once
// UPnP IGD port mapping: asks the home router to forward a UDP port to this PC, and reports the
// router's public (external) IP. Plain SSDP discovery + SOAP over HTTP; no extra libraries.
// Blocking (a few seconds at most): call from a worker thread.

#include <cstdint>
#include <string>

namespace mecmp::net::upnp {

struct Result {
    bool mapped = false;
    std::string externalIp; // may be set even when mapping failed
    std::string localIp;    // this PC's address on the router's network
    std::string error;      // human-readable reason when not mapped
};

Result mapUdpPort(uint16_t port, const char* description, uint32_t leaseSeconds);
void unmapUdpPort(uint16_t port); // best effort
// When the port is already mapped (UPnP error 718): true only for a mapping to this PC (`localIp`), the only kind
// mapUdpPort replaces. Another device's mapping is never deleted.
bool mappingIsOurs(const std::string& internalClient, const std::string& localIp);

} // namespace mecmp::net::upnp
