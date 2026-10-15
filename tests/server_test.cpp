// Tests for the dedicated server's relay rules. They build on Linux:
//   g++ -std=c++17 -Isrc tests/server_test.cpp src/server/relay.cpp src/server/discord.cpp src/net/protocol.cpp -ldl -pthread -o server_test && ./server_test
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "net/protocol.h"
#include "server/discord.h"
#include "server/relay.h"

using namespace mecmp;
namespace proto = net::protocol;
using server::Addr;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

struct Sent {
    Addr to;
    std::vector<char> data;
};

struct Harness {
    std::vector<Sent> sent;
    std::vector<std::string> log;
    server::Relay relay;
    explicit Harness(server::Config cfg = {})
        : relay(cfg, 0xabcdef01,
                [this](const Addr& to, const char* d, size_t n) { sent.push_back({to, std::vector<char>(d, d + n)}); },
                [this](const std::string& l) { log.push_back(l); }) {}
    size_t sentTo(const Addr& a) const {
        size_t n = 0;
        for (const auto& s : sent) n += s.to == a;
        return n;
    }
};

static std::vector<char> hello(uint32_t session, const char* name, uint8_t flags = 0, uint32_t seq = 1) {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeHello, session, seq};
    const size_t n = proto::writeHello(buf, h, name, 0, flags);
    return std::vector<char>(buf, buf + n);
}

static std::vector<char> state(uint32_t session, uint32_t seq = 2) {
    // The relay forwards a STATE unread: a header and a few bytes are enough.
    std::vector<char> v(sizeof(proto::Header) + 8, 0);
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeState, session, seq};
    memcpy(v.data(), &h, sizeof(h));
    return v;
}

static const Addr A{0x0a000001, 5000}, B{0x0a000002, 5000}, C{0x0a000003, 5000};

static void testForwardsToOthers() {
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    auto s = state(1);
    t.relay.onPacket(A, s.data(), s.size(), 10);
    CHECK(t.sentTo(B) == 1);
    CHECK(t.sentTo(A) == 0);
    CHECK(t.relay.members().size() == 2);
}

static void testReactForwarded() {
    // A quick answer is a session packet like a marker: it reaches the others, and an unknown type does not.
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeReact, 1, 5};
    proto::React r;
    r.id = 1;
    r.target = 2;
    r.text = "On my way";
    const size_t n = proto::writeReact(buf, h, r);
    t.relay.onPacket(A, buf, n, 10);
    CHECK(t.sentTo(B) == 1 && t.sentTo(A) == 0);
    t.sent.clear();
    const proto::Header unknown{proto::kMagic, proto::kProtocol, 99, 1, 6};
    memcpy(buf, &unknown, sizeof(unknown));
    t.relay.onPacket(A, buf, n, 20);
    CHECK(t.sentTo(B) == 0);
}

static void testSummonForwarded() {
    // A summon and its answer are session packets: they reach the others, a dedicated server forwards the
    // bytes as they are (it is the receivers that refuse to trust a vouch from it, not the server).
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeSummon, 1, 5};
    proto::Summon s;
    s.id = 1;
    s.flags = proto::kSummonHard;
    s.targetCount = 1;
    s.targets[0] = 2;
    size_t n = proto::writeSummon(buf, h, s);
    t.relay.onPacket(A, buf, n, 10);
    CHECK(t.sentTo(B) == 1 && t.sentTo(A) == 0);
    t.sent.clear();
    const proto::Header hr{proto::kMagic, proto::kProtocol, proto::kTypeSummonReply, 2, 6};
    proto::SummonReply r;
    r.id = 1;
    r.target = 1;
    r.result = proto::kSummonTeleporting;
    n = proto::writeSummonReply(buf, hr, r);
    t.relay.onPacket(B, buf, n, 20);
    CHECK(t.sentTo(A) == 1 && t.sentTo(B) == 0);
}

static void testHelloFlagsCleared() {
    Harness t;
    auto hB = hello(2, "bob");
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    auto hA = hello(1, "mallory", proto::kHelloHost | proto::kHelloServer);
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    CHECK(t.sentTo(B) == 1);
    for (const auto& s : t.sent)
        if (s.to == B) {
            std::string name;
            uint32_t look = 0;
            uint8_t flags = 0xff;
            proto::readHello(s.data.data() + sizeof(proto::Header), s.data.size() - sizeof(proto::Header), name, look, &flags);
            CHECK(flags == 0);
            CHECK(name == "mallory");
        }
}

static void testServerHello() {
    server::Config cfg;
    cfg.name = "Beat Link";
    Harness t(cfg);
    auto hA = hello(1, "alice");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.sent.clear();
    t.relay.tick(0); // a newcomer is greeted at once
    CHECK(t.sentTo(A) == 1);
    proto::Header h;
    memcpy(&h, t.sent[0].data.data(), sizeof(h));
    CHECK(h.type == proto::kTypeHello && h.session == 0xabcdef01);
    std::string name;
    uint32_t look = 1;
    uint8_t flags = 0;
    proto::readHello(t.sent[0].data.data() + sizeof(h), t.sent[0].data.size() - sizeof(h), name, look, &flags);
    CHECK(name == "Beat Link" && look == 0 && flags == (proto::kHelloHost | proto::kHelloServer));
    t.sent.clear();
    t.relay.tick(500); // not due yet
    CHECK(t.sent.empty());
    t.relay.tick(1000);
    CHECK(t.sentTo(A) == 1);
}

static void testSessionBinding() {
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    // C claims alice's id: refused, nothing forwarded, no new member.
    auto spoof = state(1);
    t.relay.onPacket(C, spoof.data(), spoof.size(), 10);
    CHECK(t.sent.empty());
    CHECK(t.relay.members().size() == 2);
    // C claims the server's id: refused.
    auto spoofServer = hello(0xabcdef01, "server?");
    t.relay.onPacket(C, spoofServer.data(), spoofServer.size(), 10);
    CHECK(t.relay.members().size() == 2);
    // B sends a STATE with alice's id: dropped.
    t.relay.onPacket(B, spoof.data(), spoof.size(), 10);
    CHECK(t.sent.empty());
    // B restarts its game: a HELLO with a new id rebinds it.
    auto hB2 = hello(7, "bob");
    t.relay.onPacket(B, hB2.data(), hB2.size(), 20);
    CHECK(t.relay.members()[1].session == 7);
    CHECK(t.sentTo(A) == 1);
    // ...but not to an id someone else has.
    auto hB3 = hello(1, "bob");
    t.relay.onPacket(B, hB3.data(), hB3.size(), 30);
    CHECK(t.relay.members()[1].session == 7);
}

static void testBadPackets() {
    Harness t;
    auto hB = hello(2, "bob");
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    auto s = state(1);
    std::vector<char> wrongProto = s;
    reinterpret_cast<proto::Header*>(wrongProto.data())->protocol = proto::kProtocol + 1;
    t.relay.onPacket(A, wrongProto.data(), wrongProto.size(), 0);
    std::vector<char> unknownType = s;
    reinterpret_cast<proto::Header*>(unknownType.data())->type = 99;
    t.relay.onPacket(A, unknownType.data(), unknownType.size(), 0);
    t.relay.onPacket(A, s.data(), 8, 0); // shorter than a header
    std::vector<char> huge(proto::kMaxPacket + 1, 0);
    memcpy(huge.data(), s.data(), sizeof(proto::Header));
    t.relay.onPacket(A, huge.data(), huge.size(), 0);
    CHECK(t.sent.empty());
    CHECK(t.relay.members().size() == 1);
    CHECK(t.relay.stats().dropped == 4);
}

static void testFullAndTimeout() {
    server::Config cfg;
    cfg.maxPlayers = 2;
    Harness t(cfg);
    auto hA = hello(1, "a"), hB = hello(2, "b"), hC = hello(3, "c");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.relay.onPacket(C, hC.data(), hC.size(), 0);
    CHECK(t.relay.members().size() == 2);
    // A keeps talking, B goes silent.
    for (uint32_t ms = 1000; ms <= 12000; ms += 1000) {
        auto s = state(1, ms);
        t.relay.onPacket(A, s.data(), s.size(), ms);
        t.relay.tick(ms);
    }
    CHECK(t.relay.members().size() == 1);
    t.relay.onPacket(C, hC.data(), hC.size(), 12100); // room again
    CHECK(t.relay.members().size() == 2);
}

static void testRateLimit() {
    server::Config cfg;
    cfg.ratePerSec = 10;
    cfg.burst = 5;
    Harness t(cfg);
    auto hB = hello(2, "b");
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    for (int i = 0; i < 20; ++i) {
        auto s = state(1, 10 + i);
        t.relay.onPacket(A, s.data(), s.size(), 0); // 20 at once: only the burst gets through
    }
    CHECK(t.sentTo(B) == 5);
    t.sent.clear();
    auto s = state(1, 100);
    t.relay.onPacket(A, s.data(), s.size(), 1000); // a second later: 10 more allowed
    CHECK(t.sentTo(B) == 1);
}

static void testKickAndSay() {
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    CHECK(t.relay.kick("alice", 100));
    CHECK(t.relay.members().size() == 1);
    t.relay.onPacket(A, hA.data(), hA.size(), 200); // blocked for a while
    CHECK(t.relay.members().size() == 1);
    t.relay.onPacket(A, hA.data(), hA.size(), 100 + 60000);
    CHECK(t.relay.members().size() == 2);
    CHECK(t.relay.kick("00000002", 60200)); // by id
    CHECK(!t.relay.kick("nobody", 60200));
    t.sent.clear();
    t.relay.say("hello\nall");
    int chats = 0;
    for (uint32_t ms = 60250; ms < 60500; ms += 50) {
        t.relay.tick(ms);
    }
    for (const auto& s : t.sent) {
        proto::Header h;
        memcpy(&h, s.data.data(), sizeof(h));
        if (h.type != proto::kTypeChat) continue;
        ++chats;
        uint32_t id = 0;
        std::string text;
        CHECK(proto::readChat(s.data.data() + sizeof(h), s.data.size() - sizeof(h), id, text));
        CHECK(text == "helloall");
    }
    CHECK(chats == 3); // one player left, 3 copies
}

static void testChatLoggedOnce() {
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    char buf[proto::kMaxPacket];
    for (uint32_t seq = 10; seq < 13; ++seq) {
        const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeChat, 1, seq};
        const size_t n = proto::writeChat(buf, h, 42, "hi");
        t.relay.onPacket(A, buf, n, seq);
    }
    int logged = 0;
    for (const auto& l : t.log) logged += l.find("chat 'alice': hi") != std::string::npos;
    CHECK(logged == 1);
}

static void testMirror() {
    server::Config cfg;
    cfg.mirror = true;
    Harness t(cfg);
    auto hA = hello(1, "alice");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    CHECK(t.sentTo(A) == 1); // its own HELLO back, as the mirror
    t.sent.clear();
    char buf[proto::kMaxPacket];
    net::PlayerState st;
    st.pos[0] = 10.f;
    proto::Encoder enc;
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeState, 1, 5};
    const size_t n = proto::writeState(buf, h, st, enc);
    t.relay.onPacket(A, buf, n, 10);
    CHECK(t.sentTo(A) == 1);
    proto::Header back;
    memcpy(&back, t.sent[0].data.data(), sizeof(back));
    CHECK(back.session == server::mirrorSession(1));
    proto::Decoder dec;
    net::PlayerState out;
    bool full = false;
    CHECK(proto::readState(t.sent[0].data.data() + sizeof(back), t.sent[0].data.size() - sizeof(back), dec, out, full));
    CHECK(out.pos[0] == 13.f);
}

static int chatsTo(const Harness& t, const Addr& a, std::string* last = nullptr) {
    int n = 0;
    for (const auto& s : t.sent) {
        proto::Header h;
        memcpy(&h, s.data.data(), sizeof(h));
        if (s.to != a || h.type != proto::kTypeChat) continue;
        ++n;
        uint32_t id = 0;
        std::string text;
        if (last && proto::readChat(s.data.data() + sizeof(h), s.data.size() - sizeof(h), id, text)) *last = text;
    }
    return n;
}

static void testKickBlocksWholeAddress() {
    // A kicked player is refused from the same address on any port for the kick time, not just the old port.
    Harness t;
    auto hA = hello(1, "alice");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    CHECK(t.relay.kick("alice", 100));
    const Addr A2{A.ip, 7000};
    t.relay.onPacket(A2, hA.data(), hA.size(), 200);
    CHECK(t.relay.members().empty());
    t.relay.onPacket(A2, hA.data(), hA.size(), 100 + 60000); // after the kick time: welcome again
    CHECK(t.relay.members().size() == 1);
}

static void testLogBudget() {
    // One player cannot fill the log: its chat lines are cut to a few per window, with one note.
    Harness t;
    auto hA = hello(1, "alice");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.log.clear();
    for (uint32_t i = 0; i < 20; ++i) {
        const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeChat, 1, i + 2};
        char buf[proto::kMaxPacket];
        const size_t n = proto::writeChat(buf, h, 1000 + i, "line " + std::to_string(i));
        t.relay.onPacket(A, buf, n, 100 + i);
    }
    int chats = 0, notes = 0;
    for (const std::string& l : t.log) {
        chats += l.rfind("chat 'alice'", 0) == 0;
        notes += l.find("logging too fast") != std::string::npos;
    }
    CHECK(chats == 5);
    CHECK(notes == 1);
}

static void testSessionGrace() {
    // A player who goes silent keeps its session id: another address cannot take it for the grace time,
    // its own address can come back, and after that the id is free.
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    for (uint32_t ms = 1000; ms <= 70000; ms += 1000) { // bob keeps talking; alice goes silent
        t.relay.onPacket(B, hB.data(), hB.size(), ms);
        t.relay.tick(ms);
        if (ms == 11000) {
            CHECK(t.relay.members().size() == 1 && t.relay.members()[0].addr == B); // alice timed out
            const Addr C{0x0a000003, 5000};
            t.relay.onPacket(C, hA.data(), hA.size(), ms); // an impostor with alice's id: refused
            CHECK(t.relay.members().size() == 1);
            t.relay.onPacket(A, hA.data(), hA.size(), ms); // alice itself, from its own address: back
            CHECK(t.relay.members().size() == 2);
            // Alice is silent from here on: she times out at 61000 (last heard at 11000), so her id is
            // reserved until 61000 + 60000 = 121000.
        }
    }
    // Still reserved for alice at 70500 (until 121000): the impostor is refused.
    const Addr C{0x0a000003, 5000};
    t.relay.onPacket(C, hA.data(), hA.size(), 70500);
    bool cAdmitted = false;
    for (const auto& m : t.relay.members()) cAdmitted = cAdmitted || m.addr == C;
    CHECK(!cAdmitted);
    t.relay.tick(130000); // after the grace time
    t.relay.onPacket(C, hA.data(), hA.size(), 130000);
    cAdmitted = false;
    for (const auto& m : t.relay.members()) cAdmitted = cAdmitted || m.addr == C;
    CHECK(cAdmitted);
}

static void testActivityLog() {
    // The message ids of a session's join and leave posts: only real message ids are kept, in order.
    mecmp::server::discord::ActivityLog log;
    log.add("123456789012345678");
    log.add("not an id");
    log.add("987654321098765432");
    CHECK(log.size() == 2);
    const auto ids = log.take();
    CHECK(ids.size() == 2 && ids[0] == "123456789012345678" && ids[1] == "987654321098765432");
    CHECK(log.size() == 0 && log.take().empty());
}

static void testSharedNames() {
    // Two players with the same name: kick and ban refuse the name (it would hit one of them at random);
    // the id still works.
    Harness t;
    auto hA = hello(1, "dup"), hB = hello(2, "dup");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    CHECK(t.relay.matchPlayers("dup") == 2);
    CHECK(!t.relay.kick("dup", 100) && t.relay.members().size() == 2);
    CHECK(t.relay.ban("dup") == 0 && t.relay.bans().empty());
    CHECK(t.relay.kick("00000002", 100) && t.relay.members().size() == 1);
    CHECK(t.relay.members()[0].addr == A);
}

static void testBans() {
    Harness t;
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    CHECK(t.relay.ban("alice") == A.ip);
    CHECK(t.relay.members().size() == 1);
    t.sent.clear();
    t.relay.onPacket(A, hA.data(), hA.size(), 100); // refused, told why (HELLO + the line twice)
    CHECK(t.relay.members().size() == 1);
    std::string text;
    CHECK(chatsTo(t, A, &text) == 2);
    CHECK(text == "You are banned from this server.");
    t.sent.clear();
    t.relay.onPacket(A, hA.data(), hA.size(), 5000); // not told again so soon
    CHECK(t.sent.empty());
    const Addr A2{A.ip, 6000}; // another port, same address: still banned
    t.relay.onPacket(A2, hA.data(), hA.size(), 5000);
    CHECK(t.relay.members().size() == 1);
    CHECK(t.relay.unban("10.0.0.1"));
    CHECK(!t.relay.unban("10.0.0.1"));
    t.relay.onPacket(A, hA.data(), hA.size(), 6000);
    CHECK(t.relay.members().size() == 2);
    CHECK(t.relay.ban("10.0.0.9") == 0x0a000009); // an IP nobody uses yet
    CHECK(t.relay.ban("nobody") == 0);
    std::set<uint32_t> list = {B.ip};
    t.relay.setBans(list); // a reloaded bans.txt: bob goes, the 10.0.0.9 ban is gone
    CHECK(t.relay.members().size() == 1 && t.relay.members()[0].addr == A);
    CHECK(t.relay.bans().size() == 1);
    uint32_t ip = 0;
    CHECK(server::parseIp("192.168.1.20", ip) && ip == 0xc0a80114);
    CHECK(!server::parseIp("192.168.1", ip) && !server::parseIp("1.2.3.4x", ip) && !server::parseIp("256.1.1.1", ip));
}

static void testFullNoticeAndPerIp() {
    server::Config cfg;
    cfg.maxPlayers = 1;
    Harness t(cfg);
    auto hA = hello(1, "a"), hB = hello(2, "b");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.sent.clear();
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    std::string text;
    CHECK(chatsTo(t, B, &text) == 2 && text == "This server is full (1/1). Try again later.");

    server::Config cfg2;
    cfg2.maxPerIp = 2;
    Harness u(cfg2);
    for (uint16_t port = 1; port <= 3; ++port) {
        auto h = hello(100 + port, "x");
        u.relay.onPacket({A.ip, port}, h.data(), h.size(), 0);
    }
    CHECK(u.relay.members().size() == 2);
    CHECK(chatsTo(u, Addr{A.ip, 3}) == 2);
}

static void testMotd() {
    server::Config cfg;
    cfg.motd = "Welcome to Beat Link";
    Harness t(cfg);
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.relay.onPacket(A, hA.data(), hA.size(), 10); // a second HELLO: no second welcome
    t.sent.clear();
    for (uint32_t ms = 50; ms <= 300; ms += 50) t.relay.tick(ms);
    std::string text;
    CHECK(chatsTo(t, A, &text) == 3 && text == "Welcome to Beat Link");
    CHECK(chatsTo(t, B) == 3); // bob's own welcome, not alice's
}

static void testInfo() {
    server::Config cfg;
    cfg.name = "Beat Link";
    cfg.maxPlayers = 12;
    Harness t(cfg);
    auto hB = hello(2, "bob");
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.sent.clear();
    char req[proto::kMaxPacket];
    proto::Header h{proto::kMagic, static_cast<uint16_t>(proto::kProtocol + 3), proto::kTypeInfo, 0x55, 77};
    const size_t n = proto::writeInfoRequest(req, h);
    CHECK(n == proto::kInfoRequestSize);
    t.relay.onPacket(A, req, n, 10); // another protocol version: answered anyway
    CHECK(t.sent.size() == 1 && t.sent[0].to == A);
    CHECK(t.sent[0].data.size() <= n); // never more than asked with
    proto::Header ah;
    memcpy(&ah, t.sent[0].data.data(), sizeof(ah));
    CHECK(ah.type == proto::kTypeInfo && ah.seq == 77 && ah.protocol == proto::kProtocol);
    proto::ServerInfo info;
    CHECK(proto::readInfoAnswer(t.sent[0].data.data() + sizeof(ah), t.sent[0].data.size() - sizeof(ah), info));
    CHECK(info.players == 1 && info.maxPlayers == 12 && info.name == "Beat Link");
    CHECK(t.relay.members().size() == 1); // asking does not join
    t.sent.clear();
    t.relay.onPacket(A, req, 40, 20); // too short to be a request: no answer
    CHECK(t.sent.empty());
    t.relay.ban("10.0.0.1");
    t.relay.onPacket(A, req, n, 30); // banned: no answer
    CHECK(t.sent.empty());
}

// Join and leave events (the Discord webhook): named players only, with the count after the change.
static void testEvents() {
    server::Config cfg;
    cfg.timeoutMs = 1000;
    Harness t(cfg);
    std::vector<server::Event> ev;
    t.relay.setOnEvent([&ev](const server::Event& e) { ev.push_back(e); });
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    t.relay.onPacket(A, hA.data(), hA.size(), 100); // a second HELLO: no second "joined"
    auto s = state(3);
    t.relay.onPacket(C, s.data(), s.size(), 100); // not named yet: no event, now or when it leaves
    CHECK(ev.size() == 2 && ev[0].kind == server::Event::Joined && ev[0].name == "alice" && ev[0].players == 1 &&
          ev[1].name == "bob" && ev[1].players == 2);
    t.relay.onPacket(A, s.data(), 0, 900); // (nothing)
    auto sA = state(1);
    t.relay.onPacket(A, sA.data(), sA.size(), 900);
    t.relay.tick(1500); // bob and the unnamed player time out; alice was heard at 900
    CHECK(ev.size() == 3 && ev[2].kind == server::Event::Left && ev[2].name == "bob");
    CHECK(t.relay.kick("alice", 1600));
    CHECK(ev.size() == 4 && ev[3].kind == server::Event::Left && ev[3].name == "alice" && ev[3].players == 0);
}

static void testDiscordFormatting() {
    namespace dc = server::discord;
    CHECK(dc::isWebhookUrl("https://discord.com/api/webhooks/123456789/abcDEF_-xyz"));
    CHECK(dc::isWebhookUrl("https://canary.discord.com/api/webhooks/123456789/abc"));
    CHECK(!dc::isWebhookUrl("http://discord.com/api/webhooks/123456789/abc"));       // not https
    CHECK(!dc::isWebhookUrl("https://discord.com.evil.example/api/webhooks/1/abc"));  // another host
    CHECK(!dc::isWebhookUrl("https://discord.com/api/webhooks/"));                    // no id
    CHECK(!dc::isWebhookUrl("https://discord.com/api/webhooks/123456789/a b"));       // spaces
    CHECK(dc::escapeMarkdown("*bold* _x_ `c` @everyone") == "\\*bold\\* \\_x\\_ \\`c\\` @everyone");
    CHECK(dc::jsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
    const std::string body = dc::payload("hi \"there\"", "Beat Link");
    CHECK(body == "{\"content\":\"hi \\\"there\\\"\",\"username\":\"Beat Link\",\"allowed_mentions\":{\"parse\":[]}}");
    CHECK(dc::payload(std::string(3000, 'x'), "").size() < 2100); // cut to Discord's limit
    CHECK(dc::retryAfter("{\"message\": \"You are being rate limited.\", \"retry_after\": 1.25, \"global\": false}", 5) == 1.25);
    CHECK(dc::retryAfter("{}", 5) == 5);
    std::deque<std::string> lines{"one", "two", std::string(30, 'x')};
    auto posts = dc::pack(lines, 30);
    CHECK(posts.size() == 2 && posts[0] == "one\ntwo" && posts[1].size() == 30);
    // The live status message: its id from the ?wait=true answer, and the edit / post addresses.
    CHECK(dc::messageId("{\"type\": 0, \"id\": \"1295071234567890123\", \"channel_id\": \"12\"}") == "1295071234567890123");
    CHECK(dc::messageId("{\"message\": \"Unknown Webhook\"}").empty());
    CHECK(dc::messageId("{\"id\": \"12a\"}").empty());
    CHECK(dc::isMessageId("123") && !dc::isMessageId("") && !dc::isMessageId("1 2"));
    CHECK(dc::withPath("https://discord.com/api/webhooks/1/abc", "/messages/9") == "https://discord.com/api/webhooks/1/abc/messages/9");
    CHECK(dc::withPath("https://discord.com/api/webhooks/1/abc?thread_id=5", "/messages/9") ==
          "https://discord.com/api/webhooks/1/abc/messages/9?thread_id=5");
    CHECK(dc::withQuery("https://x/y", "wait=true") == "https://x/y?wait=true");
    CHECK(dc::withQuery("https://x/y?thread_id=5", "wait=true") == "https://x/y?thread_id=5&wait=true");
    // A webhook without a valid URL never starts (and logs why).
    std::vector<std::string> log;
    dc::Config bad;
    bad.url = "https://example.com/hook";
    dc::Webhook off(bad, [&log](const std::string& l) { log.push_back(l); });
    CHECK(!off.enabled() && log.size() == 1);
    off.post("ignored");
}

// A finished run: forwarded to the others, reported once (it arrives 3 times), at most one per 10 s each.
static void testResultEvents() {
    Harness t;
    std::vector<server::Event> ev;
    t.relay.setOnEvent([&ev](const server::Event& e) { if (e.kind == server::Event::Result) ev.push_back(e); });
    auto hA = hello(1, "alice"), hB = hello(2, "bob");
    t.relay.onPacket(A, hA.data(), hA.size(), 0);
    t.relay.onPacket(B, hB.data(), hB.size(), 0);
    char buf[proto::kMaxPacket];
    proto::Result r;
    r.id = 5;
    r.kind = proto::kRunDash;
    r.value = 61000;
    r.title = "Nomad's Run";
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeResult, 1, 3};
    size_t n = proto::writeResult(buf, h, r);
    t.sent.clear();
    for (int i = 0; i < 3; ++i) t.relay.onPacket(A, buf, n, 100 + static_cast<uint32_t>(i) * 33);
    CHECK(t.sentTo(B) == 3); // forwarded like any packet
    CHECK(ev.size() == 1 && ev[0].name == "alice" && ev[0].result.value == 61000 && ev[0].result.title == "Nomad's Run");
    r.id = 6;
    n = proto::writeResult(buf, h, r);
    t.relay.onPacket(A, buf, n, 2000); // another run 2 s later: flooding, not reported
    CHECK(ev.size() == 1);
    r.id = 7;
    n = proto::writeResult(buf, h, r);
    t.relay.onPacket(A, buf, n, 12000);
    CHECK(ev.size() == 2);
}

int main() {
    testResultEvents();
    testEvents();
    testDiscordFormatting();
    testInfo();
    testBans();
    testSharedNames();
    testKickBlocksWholeAddress();
    testSessionGrace();
    testActivityLog();
    testLogBudget();
    testFullNoticeAndPerIp();
    testMotd();
    testMirror();
    testForwardsToOthers();
    testReactForwarded();
    testSummonForwarded();
    testHelloFlagsCleared();
    testServerHello();
    testSessionBinding();
    testBadPackets();
    testFullAndTimeout();
    testRateLimit();
    testKickAndSay();
    testChatLoggedOnce();
    if (g_failures) {
        std::printf("server tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("server tests: all passed\n");
    return 0;
}
