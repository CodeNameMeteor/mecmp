// Tests for the host setup in net/net.cpp (hostSetup) with a router stand-in: no network is used. Windows only, like
// the other tests that compile net.cpp:
//   cl /std:c++17 /EHsc /DNOMINMAX /Isrc tests/host_setup_test.cpp src/core/config.cpp src/core/log.cpp
//      src/net/joincode.cpp src/net/protocol.cpp /link ws2_32.lib
#include "../src/net/net.cpp"

#include <cstdio>

// The router: maps the port, says this PC's address, and (like some routers) not its external one.
namespace mecmp::net::upnp {
Result mapUdpPort(uint16_t, const char*, uint32_t) {
    Result r;
    r.mapped = true;
    r.localIp = "192.168.1.20";
    return r;
}
void unmapUdpPort(uint16_t) {}
bool mappingIsOurs(const std::string& internalClient, const std::string& localIp) {
    return !internalClient.empty() && internalClient == localIp;
}
} // namespace mecmp::net::upnp

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main() {
    using namespace mecmp::net;
    WSADATA wsa{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    g_run = true;
    g_role = Role::Host;
    g_generation = 3;
    g_routerWanted = 47800;
    // What a STUN server answered (as if the reply had already arrived): 8.8.4.4, seen from port 50000.
    g_stunIp = 0x08080404;
    g_stunPort = 50000;
    beginHostStatus(47800, true);
    hostSetup(3, 47800, true);
    const HostStatus s = hostStatus();
    // Mapped but no external address from the router: the join code comes from STUN, with the mapped port.
    CHECK(s.upnp == UpnpState::Mapped);
    CHECK(s.address == PublicAddressState::Available);
    CHECK(s.joinCode == joincode::encode(0x08080404, 47800));
    CHECK(!s.lanCode.empty());
    g_run = false;
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    WSACleanup();
    std::printf("host setup tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
