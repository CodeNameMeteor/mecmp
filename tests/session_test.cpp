#include "../src/net/net.cpp"

#include <cstdio>

using namespace mecmp::net;
namespace proto = mecmp::net::protocol;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static sockaddr_in address(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

static void resetSession(Role role = Role::Join) {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); WSACleanup(); }
    WSADATA wsa{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(g_sock != INVALID_SOCKET);
    g_role = role;
    g_run = true;
    g_stopRequested = false;
    g_session = 100;
    g_hostSession = 200;
    g_hostAddr = address(47801);
    g_sendTo = {g_hostAddr};
    g_members.clear();
    g_reserved.clear();
    g_remotes.clear();
    g_connectionNotices.clear();
    g_hostHeartbeat = {};
    g_hostRoundTrip = {};
    g_heardFromHost = false;
    g_hostLost = false;
    g_serverName.clear();
    g_name = "Local";
}

static void helloFrom(uint32_t sender, const char* name, uint8_t flags = 0) {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeHello, sender, 99999};
    const auto n = proto::writeHello(buf, h, name, 0, flags);
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
}

static void stateFrom(uint32_t sender, uint32_t seq, float x) {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeState, sender, seq};
    PlayerState state;
    state.pos[0] = x;
    state.timeMs = seq * 33;
    state.flags = kStateInLevel;
    const auto n = proto::writeState(buf, h, state);
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
}

static void departureFrom(uint32_t sender, uint32_t player, proto::DepartureReason reason) {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeDeparture, sender, 1};
    const auto n = proto::writeDeparture(buf, h, {player, reason});
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
}

static void testPlayerListAndLoss() {
    resetSession();
    helloFrom(200, "Server", proto::kHelloHost | proto::kHelloServer);
    helloFrom(300, "Other");
    const auto list = players();
    CHECK(list.size() == 2 && list[0].self && list[0].name == "Local");
    CHECK(!list[1].self && list[1].id == 300 && !list[1].lossKnown && list[1].pingMs == -1);
    CHECK(connectedToHost());
    stateFrom(300, 10, 1);
    helloFrom(300, "Other");
    stateFrom(300, 11, 2);
    CHECK(g_remotes.at(300).stateSequence.last == 11);
    CHECK(g_remotes.at(300).loss.percent(nowMs()) == 0);
    stateFrom(300, 9, 3);
    CHECK(g_remotes.at(300).stateSequence.last == 11);
    stateFrom(300, 13, 4);
    CHECK(g_remotes.at(300).loss.percent(nowMs()) > 0);
    g_hostHeartbeat.at = GetTickCount() - kConnectionTimeoutMs;
    CHECK(!connectedToHost());
    helloFrom(200, "Server", proto::kHelloHost | proto::kHelloServer);
    CHECK(connectedToHost());
}

static void testDepartureAuthority() {
    resetSession();
    helloFrom(200, "Host", proto::kHelloHost);
    helloFrom(300, "Other");
    takeConnectionNotices();
    departureFrom(200, 400, proto::DepartureReason::Left);
    helloFrom(400, "Late hello");
    CHECK(g_remotes.at(400).departed && takeConnectionNotices().empty());
    departureFrom(300, 300, proto::DepartureReason::Left);
    CHECK(!g_remotes.at(300).departed && takeConnectionNotices().empty());
    departRemote(300, proto::DepartureReason::TimedOut);
    takeConnectionNotices();
    departureFrom(200, 300, proto::DepartureReason::Left);
    CHECK(g_remotes.at(300).departed && players().size() == 2);
    CHECK(g_remotes.at(300).departureReason == proto::DepartureReason::Left);
    CHECK(takeConnectionNotices().empty());
    departureFrom(200, 300, proto::DepartureReason::Left);
    helloFrom(300, "Other");
    CHECK(takeConnectionNotices().empty() && g_remotes.at(300).departed);
    departureFrom(200, 100, proto::DepartureReason::Kicked);
    CHECK(!running() && !connectedToHost() && players().empty());
    CHECK(takeConnectionNotices() == std::vector<std::string>{"You were kicked"});
}

static void testHostDepartureAndSequence() {
    resetSession(Role::Host);
    g_hostSession = 0;
    Member member;
    member.addr = g_hostAddr;
    member.session = 300;
    g_members.push_back(member);
    helloFrom(300, "Other");
    stateFrom(300, UINT32_MAX, 1);
    stateFrom(300, 0, 2);
    stateFrom(300, UINT32_MAX - 1, 3);
    CHECK(g_members[0].stateSequence.last == 0);
    CHECK(g_remotes.at(300).stateSequence.last == 0);
    CHECK(g_remotes.at(300).loss.percent(nowMs()) == 0);
    takeConnectionNotices();
    departureFrom(300, 100, proto::DepartureReason::Left);
    CHECK(g_members.size() == 1);
    departureFrom(300, 300, proto::DepartureReason::Left);
    CHECK(g_members.empty() && g_sendTo.empty());
    CHECK(takeConnectionNotices() == std::vector<std::string>{"Other left the session"});
}

static void testSessionClosed() {
    resetSession();
    helloFrom(200, "Server", proto::kHelloHost | proto::kHelloServer);
    departureFrom(200, 200, proto::DepartureReason::SessionClosed);
    CHECK(!running() && !connectedToHost());
    CHECK(takeConnectionNotices() == std::vector<std::string>{"Session closed"});
}

static void testRoundTripsAndProbeBudget() {
    resetSession();
    helloFrom(200, "Server", proto::kHelloHost | proto::kHelloServer);
    helloFrom(300, "Other");
    const DWORD now = GetTickCount();
    g_hostRoundTrip.request(11, now - 50);
    g_remotes.at(300).roundTrip.request(12, now - 75);
    char buf[proto::kMaxPacket];
    proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeProbe, 200, 1};
    auto n = proto::writeProbe(buf, h, {100, 99, true});
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
    CHECK(players()[0].pingMs == -1);
    n = proto::writeProbe(buf, h, {100, 11, true});
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
    CHECK(players()[0].pingMs >= 50);
    h.session = 300;
    n = proto::writeProbe(buf, h, {100, 12, true});
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
    CHECK(players()[1].pingMs >= 75);
    resetSession(Role::Host);
    g_hostSession = 0;
    g_sendTo.clear();
    for (uint32_t id = 1; id <= 64; ++id) {
        Remote r;
        r.id = id;
        r.announced = true;
        g_remotes[id] = r;
        Member m;
        m.session = id;
        m.addr = address(static_cast<uint16_t>(48000 + id));
        g_members.push_back(m);
    }
    probePeers();
    probePeers();
    size_t pending = 0;
    for (const auto& entry : g_remotes) pending += entry.second.roundTrip.pending;
    CHECK(pending == 1);
}

int main() {
    testPlayerListAndLoss();
    testDepartureAuthority();
    testHostDepartureAndSequence();
    testSessionClosed();
    testRoundTripsAndProbeBudget();
    g_run = false;
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    WSACleanup();
    if (!failures) std::printf("session tests: all passed\n");
    return failures ? 1 : 0;
}
