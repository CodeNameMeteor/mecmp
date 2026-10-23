#include "../src/net/net.cpp"

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main() {
    using namespace mecmp::net;
    g_run = true;
    g_role = Role::Host;
    g_generation = 10;
    beginHostStatus(47800, true);
    auto status = hostStatus();
    CHECK(status.active && status.port == 47800);
    CHECK(status.upnp == UpnpState::Pending && status.address == PublicAddressState::Pending);

    upnp::Result failed;
    failed.error = "no UPnP router answered";
    CHECK(publishRouterResult(10, 47800, failed, true));
    CHECK(publishHostResult(10, 47800, 0, 47800, false, false));
    status = hostStatus();
    CHECK(status.active && status.joinCode.empty());
    CHECK(status.address == PublicAddressState::Unavailable); // Completed failure must stop showing preparation.
    CHECK(status.upnp == UpnpState::Failed && status.upnpError == failed.error);

    g_generation = 11;
    beginHostStatus(47801, false);
    status = hostStatus();
    CHECK(status.upnp == UpnpState::Disabled);
    CHECK(!publishRouterResult(10, 47800, failed, true));
    CHECK(!publishHostResult(10, 47800, 0x08080808, 47800, false, false));
    CHECK(hostStatus().port == 47801 && hostStatus().address == PublicAddressState::Pending);

    upnp::Result mapped;
    mapped.mapped = true;
    mapped.localIp = "192.168.1.2";
    CHECK(publishRouterResult(11, 47801, mapped, true));
    CHECK(publishHostResult(11, 47801, 0x08080808, 50000, false, false));
    status = hostStatus();
    uint32_t ip = 0; uint16_t port = 0;
    CHECK(status.upnp == UpnpState::Mapped && status.address == PublicAddressState::Available);
    CHECK(joincode::decode(status.joinCode, ip, port) && ip == 0x08080808 && port == 50000);
    CHECK(joincode::decode(status.lanCode, ip, port) && ip == 0xc0a80102 && port == 47801);
    CHECK(joinCode() == status.joinCode && lanCode() == status.lanCode);
    CHECK(status.publicAddress == "8.8.8.8:50000");
    CHECK(publishHostResult(11, 47801, 0x08080808, 50000, true, false));
    CHECK(hostStatus().additionalNat);

    g_generation = 12;
    beginHostStatus(47801, true); // Rehosting on the same port must still retire the old setup.
    CHECK(!publishRouterResult(11, 47801, failed, true));
    CHECK(!publishHostResult(11, 47801, 0x08080808, 50000, false, false));
    CHECK(publishRouterResult(12, 47801, mapped, true));
    CHECK(publishHostResult(12, 47801, 0, 47801, true, true));
    status = hostStatus();
    CHECK(status.upnp == UpnpState::Mapped && status.address == PublicAddressState::PrivateNetwork);
    CHECK(status.joinCode.empty() && !status.lanCode.empty());

    g_role = Role::Join;
    CHECK(!hostStatus().active);
    CHECK(!publishHostResult(12, 47801, 0x08080808, 47801, false, false));
    g_role = Role::Host; g_run = false;
    CHECK(!hostStatus().active);
    CHECK(!publishRouterResult(12, 47801, mapped, true));
    CHECK(!publishHostResult(12, 47801, 0x08080808, 47801, false, false));
    std::printf("hosting status tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
