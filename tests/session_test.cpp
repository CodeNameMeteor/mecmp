#include "../src/net/net.cpp"
#include "server/relay.h"

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
    g_ownMembership = {};
    g_ownMembershipHost = 0;
    g_presenceHost = 0;
    g_departures.clear();
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
    proto::Encoder enc;
    const auto n = proto::writeState(buf, h, state, enc);
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
}

static void departureFrom(uint32_t sender, uint32_t player, proto::DepartureReason reason, uint32_t seq = 1) {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeDeparture, sender, seq};
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
    CHECK(g_remotes.at(300).stateSeq.last == 11);
    CHECK(g_remotes.at(300).loss.percent(nowMs()) == 0);
    stateFrom(300, 9, 3);
    CHECK(g_remotes.at(300).stateSeq.last == 11);
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
    CHECK(g_members[0].stateSeq.last == 0);
    CHECK(g_remotes.at(300).stateSeq.last == 0);
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

static void presenceFrom(uint32_t host, uint32_t player, uint32_t seq) {
    char buf[sizeof(proto::Header) + sizeof(player)];
    const proto::Header h{proto::kMagic, proto::kProtocol, 18, host, seq};
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &player, sizeof(player));
    handlePacket(buf, sizeof(buf), g_hostAddr);
}

static void testOwnTimeoutKeepsReconnecting() {
    resetSession();
    helloFrom(200, "Host", proto::kHelloHost);
    departureFrom(200, 100, proto::DepartureReason::TimedOut, 5);
    CHECK(running() && !g_stopRequested);
    helloFrom(200, "Host", proto::kHelloHost);
    CHECK(running() && connectedToHost());
}

static void testConfirmedReadmissionAndOrdering() {
    resetSession();
    helloFrom(200, "Host", proto::kHelloHost);
    helloFrom(300, "Other");
    presenceFrom(200, 100, 10);
    departureFrom(200, 100, proto::DepartureReason::TimedOut, 9);
    CHECK(running() && connectedToHost());
    presenceFrom(200, 400, 21);
    helloFrom(400, "Fresh admission");
    departureFrom(200, 400, proto::DepartureReason::TimedOut, 20);
    CHECK(g_remotes.at(400).announced && !g_remotes.at(400).departed);
    departureFrom(200, 300, proto::DepartureReason::Kicked, 20);
    CHECK(g_remotes.at(300).departed);
    presenceFrom(300, 300, 21);
    helloFrom(300, "Old hello");
    CHECK(g_remotes.at(300).departed);
    presenceFrom(200, 300, 21);
    helloFrom(300, "Other");
    CHECK(g_remotes.at(300).announced && !g_remotes.at(300).departed);
    departureFrom(200, 300, proto::DepartureReason::Kicked, 20);
    CHECK(!g_remotes.at(300).departed);
    departureFrom(200, 300, proto::DepartureReason::Left, 22);
    presenceFrom(200, 300, 21);
    helloFrom(300, "Old hello");
    CHECK(g_remotes.at(300).departed);
    presenceFrom(200, 300, 22);
    CHECK(g_remotes.at(300).departed); // duplicate sequence cannot change departed to active
}

static void testMalformedTrafficAndRecovery() {
    resetSession();
    helloFrom(200, "Host", proto::kHelloHost);
    helloFrom(300, "Other");
    takeConnectionNotices();
    g_hostHeartbeat.at = GetTickCount() - kConnectionTimeoutMs;
    g_hostLost = true;
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeDeparture, 300, 1};
    const auto n = proto::writeDeparture(buf, h, {300, proto::DepartureReason::Left});
    handlePacket(buf, static_cast<int>(n), g_hostAddr);
    CHECK(!connectedToHost() && g_hostLost);
    CHECK(takeConnectionNotices().empty());
    const proto::Header badHello{proto::kMagic, proto::kProtocol, proto::kTypeHello, 200, 100000};
    memcpy(buf, &badHello, sizeof(badHello));
    handlePacket(buf, sizeof(badHello), g_hostAddr);
    CHECK(!connectedToHost() && g_hostLost && takeConnectionNotices().empty());
    departRemote(300, proto::DepartureReason::TimedOut);
    g_remotes.at(300).lastSeq = 99999;
    takeConnectionNotices();
    helloFrom(300, "Other");
    CHECK(g_remotes.at(300).announced && !g_remotes.at(300).departed);
    CHECK(connectedToHost());
}

static void testLoadingKeepsMembership() {
    resetSession();
    helloFrom(200, "Server", proto::kHelloHost | proto::kHelloServer);
    helloFrom(300, "Other");
    stateFrom(300, 1, 1);
    auto& remote = g_remotes.at(300);
    remote.loss = {};
    remote.loss.add(nowMs() - 5100, 1);
    remote.lastTick = GetTickCount() - 5100;
    CHECK(!players()[1].lossKnown);
    remote.lastTick = GetTickCount() - 6000;
    takeConnectionNotices();
    const auto list = players();
    CHECK(list.size() == 2 && list[1].id == 300 && !list[1].lossKnown);
    CHECK(remotes().empty() && takeConnectionNotices().empty());
}

static void testDelayedTimeoutDuplicate() {
    resetSession();
    helloFrom(200, "Host", proto::kHelloHost);
    helloFrom(300, "Other");
    takeConnectionNotices();
    departureFrom(200, 300, proto::DepartureReason::TimedOut);
    CHECK(g_remotes.at(300).departed);
    helloFrom(300, "Other");
    CHECK(g_remotes.at(300).announced && !g_remotes.at(300).departed);
    takeConnectionNotices();
    departureFrom(200, 300, proto::DepartureReason::TimedOut);
    CHECK(g_remotes.at(300).announced && !g_remotes.at(300).departed);
    CHECK(takeConnectionNotices().empty());
}

static void testRestartedHostDepartureSequence() {
    resetSession();
    helloFrom(200, "Host", proto::kHelloHost);
    helloFrom(300, "Other");
    departureFrom(200, 300, proto::DepartureReason::TimedOut, 500);
    helloFrom(300, "Other");
    helloFrom(201, "Restarted host", proto::kHelloHost);
    CHECK(g_hostSession == 201);
    departureFrom(201, 300, proto::DepartureReason::Left);
    CHECK(g_remotes.at(300).departed && g_remotes.at(300).departureReason == proto::DepartureReason::Left);
}

static void testGracefulStopPackets() {
    for (const auto role : {Role::Join, Role::Host}) {
        resetSession(role);
        WSADATA wsa{};
        CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        const SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(receiver != INVALID_SOCKET);
        auto target = address(0);
        CHECK(bind(receiver, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0);
        int size = sizeof(target);
        CHECK(getsockname(receiver, reinterpret_cast<sockaddr*>(&target), &size) == 0);
        const DWORD waitMs = 1000;
        CHECK(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&waitMs), sizeof(waitMs)) == 0);
        g_sendTo = {target};
        stopLocked();
        char buf[proto::kMaxPacket];
        for (int repeat = 0; repeat < 3; ++repeat) {
            const int n = recv(receiver, buf, sizeof(buf), 0);
            CHECK(n > static_cast<int>(sizeof(proto::Header)));
            if (n <= static_cast<int>(sizeof(proto::Header))) continue;
            proto::Header h{};
            memcpy(&h, buf, sizeof(h));
            proto::Departure departure;
            CHECK(h.type == proto::kTypeDeparture && h.session == 100);
            CHECK(proto::readDeparture(buf + sizeof(h), n - sizeof(h), departure));
            CHECK(departure.player == 100);
            CHECK(departure.reason == (role == Role::Host ? proto::DepartureReason::SessionClosed : proto::DepartureReason::Left));
        }
        CHECK(!running() && !connectedToHost());
        closesocket(receiver);
        WSACleanup();
    }
    resetSession();
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

struct RelayLink {
    struct Wire { mecmp::server::Addr to; std::vector<char> bytes; };
    const mecmp::server::Addr local{0x0a000001, 5000}, other{0x0a000002, 5000};
    std::vector<Wire> wire;
    mecmp::server::Relay relay;
    RelayLink() : relay({}, 200,
        [this](const mecmp::server::Addr& to, const char* d, size_t n) { wire.push_back({to, {d, d + n}}); },
        [](const std::string&) {}) {}
    void join(mecmp::server::Addr addr, uint32_t id, uint32_t now, uint32_t seq) {
        char buf[proto::kMaxPacket];
        proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeHello, id, seq};
        const size_t n = proto::writeHello(buf, h, id == 100 ? "Local" : "Other", 0);
        const size_t before = wire.size();
        relay.onPacket(addr, buf, n, now);
        for (size_t i = before; i < wire.size(); ++i) {
            proto::Header response{};
            memcpy(&response, wire[i].bytes.data(), sizeof(response));
            if (response.type != proto::kTypeChallenge || wire[i].to != addr) continue;
            uint64_t cookie = 0;
            CHECK(proto::readCookie(wire[i].bytes.data() + sizeof(response), wire[i].bytes.size() - sizeof(response), cookie));
            char answer[proto::kMaxPacket];
            h.type = proto::kTypeAnswer;
            relay.onPacket(addr, answer, proto::writeCookie(answer, h, cookie), now);
            relay.onPacket(addr, buf, n, now);
            break;
        }
    }
    void deliver() {
        for (const auto& msg : wire)
            if (msg.to == local) handlePacket(msg.bytes.data(), static_cast<int>(msg.bytes.size()), g_hostAddr);
        wire.clear();
    }
};

static void testRelayResetAndReadmission() {
    resetSession();
    RelayLink link;
    link.join(link.local, 100, 100, 1);
    link.relay.tick(100);
    link.deliver();
    CHECK(connectedToHost());
    link.relay.reset();
    link.deliver();
    CHECK(running() && !g_stopRequested);
    link.join(link.local, 100, 200, 2);
    link.relay.tick(200);
    link.deliver();
    CHECK(link.relay.members().size() == 1 && connectedToHost());
    link.relay.tick(300);
    link.deliver();
    CHECK(running() && connectedToHost());
}

static void testRelayReadmitsMissedKickAndUnban() {
    for (bool ban : {false, true}) {
        resetSession();
        RelayLink link;
        link.join(link.local, 100, 100, 1);
        link.join(link.other, 300, 100, 1);
        link.relay.tick(100);
        link.deliver();
        CHECK(players().size() == 2);
        if (ban) CHECK(link.relay.ban("Other") != 0);
        else CHECK(link.relay.kick("Other", 200));
        link.deliver(); // packets to Other are lost; it keeps the same session id
        CHECK(g_remotes.at(300).departed);
        link.join(link.other, 300, 250, 2);
        CHECK(link.relay.members().size() == 1);
        link.deliver();
        CHECK(g_remotes.at(300).departed);
        if (ban) CHECK(link.relay.unban("10.0.0.2"));
        const uint32_t now = ban ? 300 : 60201;
        link.join(link.local, 100, now, 2);
        link.join(link.other, 300, now, 3);
        link.relay.tick(now);
        link.deliver();
        link.join(link.other, 300, now + 10, 4);
        link.deliver();
        CHECK(link.relay.members().size() == 2);
        CHECK(g_remotes.at(300).announced && !g_remotes.at(300).departed);
        CHECK(players().size() == 2);
    }
}

static void testListenHostConfirmsItsOwnPlayer() {
    resetSession(Role::Host);
    g_hostSession = 0;
    const SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(receiver != INVALID_SOCKET);
    auto target = address(0);
    CHECK(bind(receiver, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0);
    int size = sizeof(target);
    CHECK(getsockname(receiver, reinterpret_cast<sockaddr*>(&target), &size) == 0);
    const DWORD waitMs = 100;
    CHECK(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&waitMs), sizeof(waitMs)) == 0);
    g_sendTo = {target};
    Member member;
    member.addr = target;
    member.session = 300;
    g_members.push_back(member);
    sendHello();
    bool ownPresence = false;
    for (int i = 0; i < 3; ++i) {
        char buf[proto::kMaxPacket];
        const int n = recv(receiver, buf, sizeof(buf), 0);
        if (n < static_cast<int>(sizeof(proto::Header))) continue;
        proto::Header h{};
        memcpy(&h, buf, sizeof(h));
        uint32_t player = 0;
        if (h.type == proto::kTypePresence && proto::readPresence(buf + sizeof(h), n - sizeof(h), player))
            ownPresence |= player == 100;
    }
    CHECK(ownPresence);
    closesocket(receiver);
    resetSession();
    helloFrom(200, "Host", proto::kHelloHost);
    presenceFrom(200, 100, 1);
    departRemote(200, proto::DepartureReason::TimedOut);
    presenceFrom(200, 200, 2);
    helloFrom(200, "Host", proto::kHelloHost);
    CHECK(g_remotes.at(200).announced && !g_remotes.at(200).departed);
}

int main() {
    testListenHostConfirmsItsOwnPlayer();
    testRelayResetAndReadmission();
    testRelayReadmitsMissedKickAndUnban();
    testOwnTimeoutKeepsReconnecting();
    testConfirmedReadmissionAndOrdering();
    testRestartedHostDepartureSequence();
    testDelayedTimeoutDuplicate();
    testGracefulStopPackets();
    testLoadingKeepsMembership();
    testMalformedTrafficAndRecovery();
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
