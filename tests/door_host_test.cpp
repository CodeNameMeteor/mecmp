#include "../src/net/net.cpp"

#include <cstdio>

using namespace mecmp::net;
static int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);                                                 \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)
static sockaddr_in address(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}
static std::vector<char> packet(uint32_t session, const doors::Message& message) {
    char data[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, doors::kPacketType, session, 500};
    memcpy(data, &h, sizeof(h));
    const auto n = doors::encode(message, data + sizeof(h), sizeof(data) - sizeof(h));
    CHECK(n != 0);
    return std::vector<char>(data, data + sizeof(h) + n);
}
static doors::Key key(float x = 0) {
    const float mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
    doors::Key k;
    CHECK(doors::makeKey(1, mat, k));
    return k;
}
static void hostHello(uint32_t session, uint32_t seq) {
    char data[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeHello, session, seq};
    const auto n = proto::writeHello(data, h, "test host", 0, proto::kHelloHost | proto::kHelloServer);
    handlePacket(data, static_cast<int>(n), g_hostAddr);
}
int main() {
    WSADATA wsa{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(g_sock != INVALID_SOCKET);
    g_run = true;
    g_role = Role::Join;
    g_session = 100;
    g_hostSession = 200;
    g_hostAddr = address(47001);
    const float pos[3] = {0, 0, 0};
    publishDoorInterest(1, pos, true);
    doors::Message state;
    state.op = doors::Op::State;
    state.id = 1;
    state.revision = 1;
    state.key = key();
    auto p = packet(200, state);
    handlePacket(p.data(), static_cast<int>(p.size()), address(47002));
    CHECK(!doorIsOpen(state.key));
    auto forged = packet(300, state);
    handlePacket(forged.data(), static_cast<int>(forged.size()), g_hostAddr);
    CHECK(!doorIsOpen(state.key));
    handlePacket(p.data(), static_cast<int>(p.size()), g_hostAddr);
    CHECK(doorIsOpen(state.key));
    CHECK(g_remotes.empty());
    char reset[proto::kMaxPacket];
    const proto::Header resetHeader{proto::kMagic, proto::kProtocol, proto::kTypeDeparture, 200, 501};
    const auto resetSize = proto::writeDeparture(reset, resetHeader, {g_session, proto::DepartureReason::SessionReset});
    handlePacket(reset, static_cast<int>(resetSize), g_hostAddr);
    CHECK(running() && !doorIsOpen(state.key));
    publishDoorInterest(1, pos, true);
    handlePacket(p.data(), static_cast<int>(p.size()), g_hostAddr);
    CHECK(!doorIsOpen(state.key)); // Old authority cannot refill the cache while reconnecting.
    publishDoorInterest(1, pos, false);
    CHECK(!doorIsOpen(state.key));
    hostHello(200, 10);
    hostHello(201, 20);
    CHECK(g_hostSession == 201);
    publishDoorInterest(1, pos, true);
    hostHello(200, 11);
    CHECK(g_hostSession == 201); // Delayed HELLO cannot revive the retired authority.
    p = packet(200, state);
    handlePacket(p.data(), static_cast<int>(p.size()), g_hostAddr);
    CHECK(!doorIsOpen(state.key));

    g_doorClient.reset();
    g_doorAuthority.reset();
    g_role = Role::Host;
    g_hostSession = 0;
    publishDoorInterest(1, pos, true);
    CHECK(publishDoorOpened(key()));
    sendDoors();
    CHECK(doorIsOpen(key()));
    CHECK(doorSyncAvailable());
    Member member;
    member.session = 200;
    member.addr = address(47002);
    g_members.push_back(member);
    doors::Message interest;
    interest.id = 1;
    interest.revision = 1;
    interest.key.world = 1;
    interest.enabled = true;
    p = packet(200, interest);
    handlePacket(p.data(), static_cast<int>(p.size()), member.addr);
    doors::Message open;
    open.op = doors::Op::Open;
    open.id = 2;
    open.key = key(1);
    p = packet(200, open);
    handlePacket(p.data(), static_cast<int>(p.size()), member.addr);
    Sleep(110);
    sendDoors();
    CHECK(doorIsOpen(open.key));
    publishDoorInterest(1, pos, false);
    CHECK(!doorIsOpen(open.key));
    CHECK(!publishDoorOpened(key(2)));
    g_run = false;
    g_members.clear();
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    WSACleanup();
    std::printf("door host tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
