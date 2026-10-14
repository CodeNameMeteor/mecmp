// Tests for the portable parts of the network code (wire format and join codes). They build on Linux:
//   g++ -std=c++17 -Isrc tests/net_test.cpp src/net/protocol.cpp src/net/joincode.cpp -o net_test && ./net_test
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "net/joincode.h"
#include "net/protocol.h"

using namespace mecmp::net;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static void testJoinCodes() {
    uint32_t ip = 0;
    uint16_t port = 0;
    // Round trip for a public address.
    const std::string code = joincode::encode(0x01020304, 47800);
    CHECK(joincode::decode(code, ip, port));
    CHECK(ip == 0x01020304 && port == 47800);
    // Codes whose first symbol is 8 or more set bits the address does not have: rejected.
    CHECK(!joincode::decode("ZZZZZ-ZZZZZ", ip, port));
    // Special addresses: unspecified, broadcast, multicast, reserved.
    CHECK(!joincode::decode(joincode::encode(0, 47800), ip, port));
    CHECK(!joincode::decode(joincode::encode(0xffffffffu, 47800), ip, port));
    CHECK(!joincode::decode(joincode::encode(0xe0000001u, 47800), ip, port));
    CHECK(!joincode::decode(joincode::encode(0xf0000001u, 47800), ip, port));
    // Port 0 is never a host.
    CHECK(!joincode::decode(joincode::encode(0x01020304, 0), ip, port));
}

static void testUtf8Prefix() {
    CHECK(mecmp::net::protocol::utf8PrefixLength("abc", 23) == 3);
    // "é" is two bytes (C3 A9): a cut between them must back up to the start of the character.
    std::string s(22, 'a');
    s += "\xC3\xA9";                                  // 24 bytes total
    CHECK(protocol::utf8PrefixLength(s, 23) == 22);   // would split the é
    CHECK(protocol::utf8PrefixLength(s, 24) == 24);
    CHECK(protocol::utf8PrefixLength(s, 0) == 0);
}

static void testHelloName() {
    char buf[protocol::kMaxPacket];
    protocol::Header h{protocol::kMagic, protocol::kProtocol, protocol::kTypeHello, 7, 1};
    const std::string name = std::string(22, 'x') + "\xC3\xA9"; // 24 bytes: only 23 fit on the wire
    const size_t n = protocol::writeHello(buf, h, name.c_str());
    CHECK(n == sizeof(protocol::Header) + protocol::kNameLen + sizeof(uint32_t) + 2); // name + look + flags + wear
    const char* field = buf + sizeof(protocol::Header);
    CHECK(std::strlen(field) == 22); // the é is dropped whole, not cut in half
}

static void testReadState() {
    protocol::Encoder enc;
    protocol::Header h{protocol::kMagic, protocol::kProtocol, protocol::kTypeState, 1, 1};
    PlayerState s{};
    s.pos[0] = 1; s.pos[1] = 2; s.pos[2] = 3; s.yaw = 0.5f; s.timeMs = 1234;
    s.boneCount = 23;
    for (int i = 0; i < 23; ++i) { s.bones[i][3] = 1; s.bones[i][7] = 1; s.bones[i][0] = i * 0.01f; }
    char buf[protocol::kMaxPacket];
    const size_t n = protocol::writeState(buf, h, s, enc);

    // A well-formed packet decodes.
    protocol::Decoder dec;
    PlayerState out;
    bool full = false;
    CHECK(protocol::readState(buf + sizeof(h), n - sizeof(h), dec, out, full));
    CHECK(out.boneCount == 23 && std::fabs(out.pos[2] - 3) < 1e-6f);

    // Non-finite or far-away positions are refused.
    for (float bad : {NAN, INFINITY, 1e30f}) {
        PlayerState b = s;
        b.pos[0] = bad;
        protocol::Encoder e2;
        char b2[protocol::kMaxPacket];
        const size_t n2 = protocol::writeState(b2, h, b, e2);
        PlayerState o2;
        bool f2 = false;
        protocol::Decoder d2;
        CHECK(!protocol::readState(b2 + sizeof(h), n2 - sizeof(h), d2, o2, f2));
    }

    // Every truncation of a valid packet is refused or decoded without reading past the end.
    for (size_t cut = 0; cut < n - sizeof(h); ++cut) {
        std::vector<char> copy(buf + sizeof(h), buf + sizeof(h) + cut);
        PlayerState o3;
        bool f3 = false;
        protocol::Decoder d3;
        d3.baseCount = 23;
        protocol::readState(copy.data(), copy.size(), d3, o3, f3);
    }

    // Random payloads never crash (run this file under -fsanitize=address,undefined to check reads).
    std::mt19937 rng(42);
    for (int i = 0; i < 200000; ++i) {
        std::vector<char> p(rng() % 200);
        for (char& c : p) c = static_cast<char>(rng());
        protocol::Decoder d;
        d.baseCount = rng() % 33;
        PlayerState o;
        bool f = false;
        protocol::readState(p.data(), p.size(), d, o, f);
    }
}

static void testClockWrap() {
    // A sender whose 32-bit millisecond clock wraps (after 49.7 days of uptime) keeps a steady clock.
    protocol::ClockUnwrap c;
    const uint32_t start = 0xFFFFFF00u;
    const double first = c.extend(start);
    CHECK(first == static_cast<double>(start));
    double prev = first;
    for (uint32_t k = 1; k <= 20; ++k) { // 33 ms steps across the wrap
        const double t = c.extend(start + k * 33u);
        CHECK(t - prev == 33.0);
        prev = t;
    }
    CHECK(prev - first == 20 * 33.0);
    // A packet that arrives late (older timestamp) steps back, it does not jump a whole wrap.
    const double late = c.extend(start + 19u * 33u);
    CHECK(prev - late == 33.0);
    // A long silence (one hour) is still a forward step.
    const double later = c.extend(start + 19u * 33u + 3600000u);
    CHECK(later - late == 3600000.0);
}

static void testAlignRotations() {
    // The same rotation sent twice can come back as q and -q; the second is flipped back to q.
    PlayerState a, b;
    a.boneCount = b.boneCount = 2;
    const float q[4] = {0.70710677f, 0.f, 0.f, 0.70710677f};
    for (int j = 0; j < 4; ++j) {
        a.bones[0][4 + j] = q[j];
        b.bones[0][4 + j] = -q[j];
        a.bones[1][4 + j] = b.bones[1][4 + j] = q[j]; // already the same sign: unchanged
    }
    protocol::alignRotations(b, a);
    for (int j = 0; j < 4; ++j) {
        CHECK(b.bones[0][4 + j] == q[j]);
        CHECK(b.bones[1][4 + j] == q[j]);
    }
    // Different bone counts: left alone.
    PlayerState c = b;
    c.boneCount = 1;
    c.bones[0][4] = -q[0];
    protocol::alignRotations(c, a);
    CHECK(c.bones[0][4] == -q[0]);
}

static void testDelayEstimator() {
    // A sender whose clock runs 1% slow: (arrival - sent) grows by 10 ms per second. The estimate
    // must follow it (an all-time minimum would fall 10 ms further behind every second).
    protocol::DelayEstimator d;
    double est = 0;
    for (int i = 0; i <= 300; ++i) { // 10 s of packets every 33 ms
        const double arrival = 1000.0 + i * 33.0;
        const double sent = (arrival - 50.0) * 0.99; // 50 ms delay, slow clock
        est = d.add(arrival + (i % 5) * 4.0, sent);   // some queueing jitter
    }
    const double trueDelta = (1000.0 + 300 * 33.0) - (1000.0 + 300 * 33.0 - 50.0) * 0.99;
    CHECK(std::fabs(est - trueDelta) < 25.0); // within the 2 s window's drift (20 ms) plus jitter
    // Queueing spikes do not raise it while a fast packet is in the window.
    protocol::DelayEstimator e;
    e.add(0, 0);
    CHECK(e.add(33, 33 - 200) == 0); // a 200 ms late packet: the earlier 0 still counts
}

static void testLossWindow() {
    protocol::LossWindow w;
    CHECK(w.percent(0) == 0.f);
    for (int i = 0; i < 90; ++i) w.add(10000 + i * 33.0, i % 10 == 9 ? 1 : 0); // 1 in 10 missed
    CHECK(std::fabs(w.percent(13000) - 100.f * 9 / 99) < 0.01f);
    CHECK(w.percent(20000) == 0.f); // old seconds no longer count
    for (int i = 0; i < 10; ++i) w.add(20000 + i * 33.0, 0);
    CHECK(w.percent(20500) == 0.f);
}

static void testHelloLook() {
    char buf[protocol::kMaxPacket];
    protocol::Header h{};
    const size_t n = protocol::writeHello(buf, h, "Meteor", 0x1234abcdu);
    std::string name;
    uint32_t look = 0;
    CHECK(protocol::readHello(buf + sizeof(h), n - sizeof(h), name, look));
    CHECK(name == "Meteor" && look == 0x1234abcdu);
    uint8_t flags = 9;
    const size_t nh = protocol::writeHello(buf, h, "Host", 5, protocol::kHelloHost);
    CHECK(protocol::readHello(buf + sizeof(h), nh - sizeof(h), name, look, &flags) && flags == protocol::kHelloHost);
    CHECK(protocol::readHello(buf + sizeof(h), nh - sizeof(h) - 2, name, look, &flags) && flags == 0); // older sender
    // Faith's outfit changes follow the flags; a sender without them (the line above) reads as none.
    uint8_t wear = 9;
    CHECK(protocol::readHello(buf + sizeof(h), nh - sizeof(h) - 1, name, look, &flags, &wear) && wear == 0);
    const uint8_t worn = protocol::kWearPrologue | protocol::kWearGridLink;
    const size_t no = protocol::writeHello(buf, h, "Host", 0, protocol::kHelloHost, worn);
    CHECK(no == nh && protocol::readHello(buf + sizeof(h), no - sizeof(h), name, look, &flags, &wear) &&
          flags == protocol::kHelloHost && wear == worn);
    CHECK(protocol::readHello(buf + sizeof(h), no - sizeof(h) - 1, name, look, &flags, &wear) && flags == protocol::kHelloHost && wear == 0);
    protocol::writeHello(buf, h, "Meteor", 0x1234abcdu);
    // An older version's HELLO (name only): look 0 = Faith.
    CHECK(protocol::readHello(buf + sizeof(h), protocol::kNameLen, name, look) && look == 0 && name == "Meteor");
    // A short packet keeps what is there.
    CHECK(protocol::readHello(buf + sizeof(h), 3, name, look) && name == "Met" && look == 0);
}

static void testChat() {
    char buf[protocol::kMaxPacket];
    protocol::Header h{};
    const size_t n = protocol::writeChat(buf, h, 77, "  hello \x1b[2Jthere\n ");
    uint32_t id = 0;
    std::string text;
    CHECK(protocol::readChat(buf + sizeof(h), n - sizeof(h), id, text));
    CHECK(id == 77 && text == "hello [2Jthere"); // control characters and outer spaces gone
    // Long text is cut to the limit without splitting a UTF-8 character.
    std::string longText(protocol::kMaxChatBytes - 1, 'a');
    longText += "\xC3\xA9";
    CHECK(protocol::cleanChatText(longText).size() == protocol::kMaxChatBytes - 1);
    // Truncated or empty messages are refused.
    CHECK(!protocol::readChat(buf + sizeof(h), 4, id, text));
    const size_t e = protocol::writeChat(buf, h, 1, "   ");
    CHECK(!protocol::readChat(buf + sizeof(h), e - sizeof(h), id, text));
    // A length byte beyond the limit or the packet is refused.
    char bad[8] = {1, 0, 0, 0, static_cast<char>(200), 'x'};
    CHECK(!protocol::readChat(bad, 6, id, text));
}

static void testKnownTypes() {
    CHECK(protocol::knownType(protocol::kTypeState) && protocol::knownType(protocol::kTypeHello));
    CHECK(protocol::knownType(protocol::kTypeChat) && protocol::knownType(protocol::kTypePing));
    CHECK(!protocol::knownType(0) && !protocol::knownType(5) && !protocol::knownType(0xffff));
}

static void testRecentIds() {
    protocol::RecentIds<4> r;
    CHECK(!r.add(0)); // 0 is never a message id
    CHECK(r.add(10) && !r.add(10)); // shown once, repeats refused
    CHECK(r.add(11) && r.add(12) && r.add(13));
    CHECK(!r.add(10)); // still remembered: 4 slots
    CHECK(r.add(14));  // pushes the oldest (10) out
    CHECK(r.add(10) && !r.add(14));
}

static void testPing() {
    char buf[protocol::kMaxPacket];
    protocol::Header h{protocol::kMagic, protocol::kProtocol, protocol::kTypePing, 7, 1};
    protocol::Ping in;
    in.id = 0x00070001;
    in.pos[0] = 12.5f; in.pos[1] = -3.25f; in.pos[2] = 900.f;
    in.rgb[0] = 10; in.rgb[1] = 200; in.rgb[2] = 30;
    const size_t n = protocol::writePing(buf, h, in);
    CHECK(n == sizeof(h) + 4 + 12 + 1 + 3 + 1 + 1 + 3 + 1); // + the label length (0), life, accent, index
    protocol::Ping out;
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out));
    CHECK(out.id == in.id && out.pos[0] == 12.5f && out.pos[1] == -3.25f && out.pos[2] == 900.f && out.flags == 0);
    CHECK(out.rgb[0] == 10 && out.rgb[1] == 200 && out.rgb[2] == 30);
    // "Remove my marker".
    in.flags = protocol::kPingClear;
    const size_t nc = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), nc - sizeof(h), out) && (out.flags & protocol::kPingClear));
    // A sender that stops after the position: no flags, the default colour.
    protocol::Ping def;
    out.rgb[0] = 1;
    CHECK(protocol::readPing(buf + sizeof(h), 16, out) && out.flags == 0 && out.rgb[0] == def.rgb[0]);
    // Flags but a cut-off colour: the colour stays the default.
    CHECK(protocol::readPing(buf + sizeof(h), 18, out) && (out.flags & protocol::kPingClear) && out.rgb[1] == def.rgb[1]);
    // Too short for a position: refused, and `out` is left alone.
    out.id = 99;
    CHECK(!protocol::readPing(buf + sizeof(h), 15, out) && out.id == 99);
    CHECK(!protocol::readPing(buf + sizeof(h), 0, out));
    // Positions that are not numbers or far outside any level: refused (as for STATE).
    for (float bad : {std::nanf(""), INFINITY, -INFINITY, 100001.f, -1e30f}) {
        protocol::Ping b = in;
        b.pos[1] = bad;
        const size_t nb = protocol::writePing(buf, h, b);
        CHECK(!protocol::readPing(buf + sizeof(h), nb - sizeof(h), out));
    }
    const float edge[3] = {100000.f, -100000.f, 0};
    CHECK(protocol::sanePosition(edge));
    // Random bytes never crash the reader, and what it accepts is always a sane position.
    std::mt19937 rng(1234);
    for (int i = 0; i < 2000; ++i) {
        char junk[64];
        for (char& ch : junk) ch = static_cast<char>(rng());
        if (i % 4 == 0) junk[20] = static_cast<char>(rng() % 12); // a short label, so the fields after it are read too
        protocol::Ping j;
        if (protocol::readPing(junk, rng() % (sizeof(junk) + 1), j)) {
            CHECK(protocol::sanePosition(j.pos));
            CHECK(j.label.size() <= protocol::kMaxPingLabelBytes && j.label == protocol::cleanPingLabel(j.label));
            CHECK(j.index < protocol::kPingCells || (j.index == protocol::kPingAllCells && (j.flags & protocol::kPingClear)));
        }
    }
}

// The marker's glyph (flags bits 1-6) and its title (after the colour).
static void testPingLabel() {
    char buf[protocol::kMaxPacket];
    protocol::Header h{protocol::kMagic, protocol::kProtocol, protocol::kTypePing, 7, 1};
    protocol::Ping in, out;
    in.id = 5;
    in.pos[0] = 1.f;
    // Glyph and title survive the wire; the clear bit is separate from the glyph bits.
    in.flags = protocol::withPingGlyph(0, 5);
    in.label = "Climb here";
    size_t n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out));
    CHECK(protocol::pingGlyph(out.flags) == 5 && !(out.flags & protocol::kPingClear) && out.label == "Climb here");
    in.flags = static_cast<uint8_t>(protocol::withPingGlyph(protocol::kPingClear, 7));
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out));
    CHECK((out.flags & protocol::kPingClear) && protocol::pingGlyph(out.flags) == 7);
    CHECK(protocol::withPingGlyph(protocol::kPingClear, 0) == protocol::kPingClear);
    for (uint8_t g = 0; g < 64; ++g) {
        CHECK(protocol::pingGlyph(protocol::withPingGlyph(0x81, g)) == g);
        CHECK((protocol::withPingGlyph(0x81, g) & 0x81) == 0x81); // the other bits stay
    }
    CHECK(protocol::pingGlyph(protocol::withPingGlyph(0, 63)) == 63);
    CHECK(protocol::pingGlyph(protocol::withPingGlyph(0, 65)) == 1); // only 6 bits: 65 -> 1, no bleed into the others
    CHECK((protocol::withPingGlyph(0, 65) & ~0x7E) == 0);
    // No title: the empty string, and the packet is the marker's old size plus the length byte.
    in.label.clear();
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out) && out.label.empty());
    // A sender from before titles stops after the colour: still a marker, no title.
    in.label = "Danger";
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), 4 + 12 + 1 + 3, out) && out.label.empty());
    // A length that promises more than is there, or more than allowed: the title is dropped, not the marker.
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h) - 5 - 2, out) && out.label.empty());
    buf[sizeof(h) + 4 + 12 + 1 + 3] = static_cast<char>(protocol::kMaxPingLabelBytes + 1);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out) && out.label.empty() && out.pos[0] == 1.f);
    // Cleaning: control characters out, blanks trimmed, cut at the limit without splitting a character.
    CHECK(protocol::cleanPingLabel("  Go\x01 this\n way  ") == "Go this way");
    CHECK(protocol::cleanPingLabel("    ").empty());
    const std::string cyr(40, 'x');
    CHECK(protocol::cleanPingLabel(cyr).size() == protocol::kMaxPingLabelBytes && protocol::kMaxPingLabelBytes == 32);
    std::string two; // 20 two-byte letters = 40 bytes: 16 whole letters fit
    for (int i = 0; i < 20; ++i) two += "\xD0\x96";
    const std::string cut = protocol::cleanPingLabel(two);
    CHECK(cut.size() == protocol::kMaxPingLabelBytes && cut.size() % 2 == 0);
    // What goes out is cleaned too, even if the caller forgot.
    in.label = std::string("A\x07") + two;
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out) && out.label.size() <= protocol::kMaxPingLabelBytes &&
          out.label.find('\x07') == std::string::npos);
}

// What follows the title: how long the marker stays, its accent colour and the cell it goes into.
// A quick answer to someone's marker.
static void testReact() {
    char buf[protocol::kMaxPacket];
    const protocol::Header h{protocol::kMagic, protocol::kProtocol, protocol::kTypeReact, 7, 1};
    protocol::React in, out;
    in.id = 11;
    in.target = 0xdeadbeefu;
    in.cell = 3;
    in.title = "Here";
    in.text = "On my way";
    size_t n = protocol::writeReact(buf, h, in);
    CHECK(protocol::knownType(protocol::kTypeReact));
    CHECK(protocol::readReact(buf + sizeof(h), n - sizeof(h), out));
    CHECK(out.id == 11 && out.target == 0xdeadbeefu && out.cell == 3 && out.title == "Here" && out.text == "On my way");
    // A marker without a title is fine; an answer without text is not.
    in.title.clear();
    n = protocol::writeReact(buf, h, in);
    CHECK(protocol::readReact(buf + sizeof(h), n - sizeof(h), out) && out.title.empty() && out.text == "On my way");
    in.text.clear();
    n = protocol::writeReact(buf, h, in);
    CHECK(!protocol::readReact(buf + sizeof(h), n - sizeof(h), out));
    // Every cut of a good packet is refused, never read out of bounds (the sanitizers watch).
    in.title = "Roof";
    in.text = "Got it";
    n = protocol::writeReact(buf, h, in);
    for (size_t cut = 0; cut + 1 < n - sizeof(h); ++cut) CHECK(!protocol::readReact(buf + sizeof(h), cut, out));
    // A cell that does not exist, and a text that claims to be longer than the packet.
    n = protocol::writeReact(buf, h, in);
    buf[sizeof(h) + 8] = static_cast<char>(protocol::kPingCells);
    CHECK(!protocol::readReact(buf + sizeof(h), n - sizeof(h), out));
    n = protocol::writeReact(buf, h, in);
    buf[n - 1 - 6] = static_cast<char>(200); // the text's length byte
    CHECK(!protocol::readReact(buf + sizeof(h), n - sizeof(h), out));
    // Over-long and dirty texts are cut and cleaned like marker titles.
    in.title = "x";
    in.text = std::string(1, char(7)) + std::string(80, 'a');
    n = protocol::writeReact(buf, h, in);
    CHECK(protocol::readReact(buf + sizeof(h), n - sizeof(h), out) && out.text.size() <= protocol::kMaxPingLabelBytes &&
          out.text.find(char(7)) == std::string::npos);
}

static void testPingTail() {
    char buf[protocol::kMaxPacket];
    protocol::Header h{protocol::kMagic, protocol::kProtocol, protocol::kTypePing, 7, 1};
    protocol::Ping in, out;
    in.id = 9;
    in.pos[2] = 4.f;
    in.label = "Roof";
    in.life = 12;
    in.accent[0] = 1; in.accent[1] = 2; in.accent[2] = 3;
    in.index = 4;
    size_t n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out));
    CHECK(out.label == "Roof" && out.life == 12 && out.accent[0] == 1 && out.accent[1] == 2 && out.accent[2] == 3 && out.index == 4);
    // A sender that stops after the title: the default time, a white accent, cell 0.
    const size_t upToLabel = 4 + 12 + 1 + 3 + 1 + in.label.size();
    CHECK(protocol::readPing(buf + sizeof(h), upToLabel, out));
    CHECK(out.label == "Roof" && out.life == 0 && out.accent[0] == 255 && out.accent[2] == 255 && out.index == 0);
    // Cut inside the accent: the accent stays white (not half of one), and so does the cell.
    CHECK(protocol::readPing(buf + sizeof(h), upToLabel + 1 + 2, out) && out.life == 12 && out.accent[0] == 255 && out.index == 0);
    // No index byte: cell 0 for a marker, all cells for a remove.
    CHECK(protocol::readPing(buf + sizeof(h), upToLabel + 1 + 3, out) && out.accent[1] == 2 && out.index == 0);
    in.flags = protocol::kPingClear;
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out) && out.index == 4);     // remove one cell
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h) - 1, out) && out.index == protocol::kPingAllCells);
    CHECK(protocol::readPing(buf + sizeof(h), 4 + 12 + 1 + 3, out) && out.index == protocol::kPingAllCells); // the first version's remove
    in.index = protocol::kPingAllCells;
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out) && out.index == protocol::kPingAllCells);
    // A cell that does not exist: cell 0 for a marker, all cells for a remove.
    in.index = 5;
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out) && out.index == protocol::kPingAllCells);
    in.flags = 0;
    in.index = 200;
    n = protocol::writePing(buf, h, in);
    CHECK(protocol::readPing(buf + sizeof(h), n - sizeof(h), out) && out.index == 0);
    // The time: 5 s steps, 0 = 30 s, never more than 5 minutes.
    CHECK(protocol::pingLifeMs(0) == 30000 && protocol::pingLifeMs(1) == 5000 && protocol::pingLifeMs(60) == 300000);
    CHECK(protocol::pingLifeMs(61) == 300000 && protocol::pingLifeMs(255) == 300000);
    CHECK(protocol::pingLifeUnits(5) == 1 && protocol::pingLifeUnits(30) == 6 && protocol::pingLifeUnits(300) == 60);
    CHECK(protocol::pingLifeUnits(0) == 1 && protocol::pingLifeUnits(-7) == 1 && protocol::pingLifeUnits(100000) == 60);
    CHECK(protocol::pingLifeUnits(12) == 2 && protocol::pingLifeUnits(13) == 3); // rounded to the nearest step
}

// Which cell a new marker takes: the same marker nearby moves, a different one sits beside it, and
// over the limit the oldest goes.
static void testPingCells() {
    protocol::PingCellView c[protocol::kPingCells];
    const float here[3] = {10.f, 0.f, 10.f}, near[3] = {11.f, 0.f, 10.f}, far[3] = {20.f, 0.f, 10.f};
    auto put = [&](int i, uint8_t glyph, const char* label, const float* pos, uint64_t order) {
        c[i].live = true;
        c[i].glyph = glyph;
        c[i].label = label;
        for (int k = 0; k < 3; ++k) c[i].pos[k] = pos[k];
        c[i].order = order;
    };
    CHECK(protocol::choosePingCell(c, 3, 0, "Here", here) == 0); // nothing yet: the first cell
    put(0, 0, "Here", here, 1);
    CHECK(protocol::choosePingCell(c, 3, 0, "Here", near) == 0);  // the same marker 1 m away: it moves
    CHECK(protocol::choosePingCell(c, 3, 0, "Here", far) == 1);   // further: a second one
    CHECK(protocol::choosePingCell(c, 3, 4, "Here", here) == 1);  // another icon on the same spot: beside it
    CHECK(protocol::choosePingCell(c, 3, 0, "Wait", here) == 1);  // another title too
    CHECK(protocol::samePingMarker(c[0], 0, "Here", near) && !protocol::samePingMarker(c[0], 0, "Here", far));
    const float edge[3] = {11.5f, 0.f, 10.f}, past[3] = {11.6f, 0.f, 10.f};
    CHECK(protocol::samePingMarker(c[0], 0, "Here", edge) && !protocol::samePingMarker(c[0], 0, "Here", past));
    // Two of the same marker in reach: the nearer one moves.
    put(1, 0, "Here", near, 2);
    const float nearer[3] = {10.9f, 0.f, 10.f};
    CHECK(protocol::choosePingCell(c, 3, 0, "Here", nearer) == 1);
    // At the limit a different marker takes the cell set longest ago; a free cell is not used.
    put(2, 2, "Danger", far, 3);
    CHECK(protocol::choosePingCell(c, 3, 5, "Race start", far) == 0);
    c[0].order = 9; // cell 0 was placed again: now cell 1 is the oldest
    CHECK(protocol::choosePingCell(c, 3, 5, "Race start", far) == 1);
    CHECK(protocol::choosePingCell(c, 4, 5, "Race start", far) == 3); // room for one more
    // A cell whose marker is gone is free again, and is not "the same marker".
    c[1].live = false;
    CHECK(protocol::choosePingCell(c, 3, 0, "Here", near) == 0);
    CHECK(protocol::choosePingCell(c, 3, 5, "Race start", far) == 1);
    // The limit is kept inside 1..kPingCells.
    CHECK(protocol::choosePingCell(c, 0, 5, "Race start", far) == 2);   // limit 1: the oldest of the live ones
    CHECK(protocol::choosePingCell(c, 99, 5, "Race start", far) == 1);
    for (int i = 0; i < protocol::kPingCells; ++i) put(i, 1, "x", far, 10 + static_cast<uint64_t>(i));
    CHECK(protocol::choosePingCell(c, 99, 5, "Race start", here) == 0); // all five in use: the oldest
}

static void testRgb() {
    uint8_t rgb[3] = {1, 2, 3};
    CHECK(protocol::parseRgb("ffd23c", rgb) && rgb[0] == 0xff && rgb[1] == 0xd2 && rgb[2] == 0x3c);
    CHECK(protocol::parseRgb("#00FF7f", rgb) && rgb[0] == 0 && rgb[1] == 0xff && rgb[2] == 0x7f);
    CHECK(protocol::formatRgb(rgb) == "00ff7f");
    for (const char* bad : {"", "#", "fff", "ffd23", "ffd23c0", "gggggg", "ff d23c", "#ffd23"})
        CHECK(!protocol::parseRgb(bad, rgb));
    CHECK(rgb[0] == 0 && rgb[1] == 0xff && rgb[2] == 0x7f); // left alone when refused
}

int main() {
    testKnownTypes();
    testRecentIds();
    testPing();
    testPingLabel();
    testReact();
    testPingTail();
    testPingCells();
    testRgb();
    testChat();
    testLossWindow();
    testHelloLook();
    testDelayEstimator();
    testAlignRotations();
    testClockWrap();
    testJoinCodes();
    testUtf8Prefix();
    testHelloName();
    testReadState();
    if (g_failures == 0) std::printf("net tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
