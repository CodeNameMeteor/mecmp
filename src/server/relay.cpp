#include "server/relay.h"
#include "net/pose_limits.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <utility>

#include "net/protocol.h"

namespace mecmp::server {
namespace proto = net::protocol;

std::string toString(const Addr& a) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (a.ip >> 24) & 0xff, (a.ip >> 16) & 0xff, (a.ip >> 8) & 0xff, a.ip & 0xff,
             a.port);
    return buf;
}

std::string ipToString(uint32_t ip) {
    const std::string s = toString({ip, 0});
    return s.substr(0, s.rfind(':'));
}

bool parseIp(const std::string& text, uint32_t& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    ip = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

std::string cleanText(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw)
        if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
    return out;
}

namespace {
std::string hex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}
} // namespace

Relay::Relay(Config cfg, uint32_t serverSession, Send send, Log log)
    : cfg_(std::move(cfg)), session_(serverSession), send_(std::move(send)), log_(std::move(log)),
      chatId_(serverSession * 2654435761u) {
    std::random_device rd;
    for (uint8_t& b : cookieKey_) b = static_cast<uint8_t>(rd());
}

Member* Relay::find(const Addr& a) {
    for (auto& m : members_)
        if (m.addr == a) return &m;
    return nullptr;
}

bool Relay::sessionTaken(uint32_t session, const Addr& except) const {
    for (const auto& r : reserved_)
        if (r.session == session && r.addr != except) return true;
    for (const auto& m : members_)
        if (m.session == session && m.addr != except) return true;
    return session == session_;
}

namespace {
constexpr int kLogBurst = 5;             // log lines per player...
constexpr uint32_t kLogWindowMs = 10000; // ...in this window
} // namespace

bool Relay::logAllowed(Member& m, uint32_t nowMs) {
    if (nowMs - m.logAt > kLogWindowMs) { m.logAt = nowMs; m.logBurst = 0; }
    if (++m.logBurst <= kLogBurst) return true;
    if (m.logBurst == kLogBurst + 1)
        log_("player " + hex(m.session) + " '" + m.name + "' is logging too fast; its lines are left out for a while");
    return false;
}

bool Relay::blocked(const Addr& a, uint32_t nowMs) {
    for (size_t i = 0; i < blocks_.size();) {
        if (static_cast<int32_t>(nowMs - blocks_[i].until) >= 0) {
            blocks_.erase(blocks_.begin() + static_cast<long>(i));
            continue;
        }
        if (blocks_[i].ip == a.ip && (blocks_[i].port == 0 || blocks_[i].port == a.port)) return true;
        ++i;
    }
    return false;
}

void Relay::drop(size_t index, const char* why) {
    const Member& m = members_[index];
    log_("player " + hex(m.session) + " '" + m.name + "' (" + toString(m.addr) + ") left: " + why + "; " +
         std::to_string(members_.size() - 1) + " connected");
    Event e{Event::Left, m.name, members_.size() - 1, {}};
    const bool tell = m.announced; // a player who never said its name was never announced either
    members_.erase(members_.begin() + static_cast<long>(index));
    if (tell && onEvent_) onEvent_(e);
}

void Relay::onPacket(const Addr& from, const char* buf, size_t n, uint32_t nowMs) {
    proto::Header h;
    if (n < sizeof(h) || n > proto::kMaxPacket) { ++stats_.dropped; return; }
    memcpy(&h, buf, sizeof(h));
    if (h.magic == proto::kMagic && h.type == proto::kTypeInfo) { // a server list asking; any protocol
        if (!bans_.count(from.ip) && proto::isInfoRequest(buf + sizeof(h), n - sizeof(h))) {
            proto::ServerInfo info;
            info.players = static_cast<uint8_t>(members_.size() > 255 ? 255 : members_.size());
            info.maxPlayers = static_cast<uint8_t>(cfg_.maxPlayers > 255 ? 255 : cfg_.maxPlayers);
            info.name = cfg_.name;
            char out[proto::kMaxPacket];
            const proto::Header ah{proto::kMagic, proto::kProtocol, proto::kTypeInfo, session_, h.seq};
            send_(from, out, proto::writeInfoAnswer(out, ah, info));
            ++stats_.infoAnswered;
        } else {
            ++stats_.dropped;
        }
        return;
    }
    if (h.magic != proto::kMagic || h.protocol != proto::kProtocol || !(proto::knownType(h.type) || h.type == proto::kTypeAnswer)) {
        ++stats_.dropped;
        return;
    }
    if (bans_.count(from.ip)) {
        ++stats_.dropped;
        notify(from, "You are banned from this server.", nowMs);
        return;
    }
    if (blocked(from, nowMs)) { ++stats_.dropped; return; }
    if (h.type == proto::kTypeAnswer) { onAnswer(from, h.session, buf + sizeof(h), n - sizeof(h), nowMs); return; }
    Member* m = find(from);
    if (!m) { // a stranger: only a HELLO is answered, with a challenge; nothing else is heard before it answers
        if (h.type != proto::kTypeHello) { ++stats_.dropped; return; }
        challenge(from, nowMs);
        return;
    }
    if (h.session != m->session) {
        // Same address, new id: the game was restarted. Only a HELLO may change it, and only to an id
        // nobody else uses.
        if (h.type != proto::kTypeHello || sessionTaken(h.session, from)) { ++stats_.dropped; ++m->dropped; return; }
        log_("player " + hex(m->session) + " '" + m->name + "' is now " + hex(h.session) + " (restarted)");
        m->session = h.session;
        m->poseDecoder = {};
    }
    // Rate limit (token bucket).
    m->tokens += (nowMs - m->refillAt) * cfg_.ratePerSec / 1000.0;
    if (m->tokens > cfg_.burst) m->tokens = cfg_.burst;
    m->refillAt = nowMs;
    if (m->tokens < 1) { ++stats_.dropped; ++m->dropped; return; }
    m->tokens -= 1;
    m->lastSeen = nowMs;
    ++m->packets;
    ++stats_.received;

    const char* payload = buf + sizeof(h);
    const size_t len = n - sizeof(h);
    char copy[proto::kMaxPacket];
    const char* out = buf;
    if (h.type == proto::kTypeState) {
        net::PlayerState state; bool full = false;
        if (!proto::readState(payload, len, m->poseDecoder, state, full)) { ++stats_.dropped; return; }
        if (full) proto::commitBase(m->poseDecoder, state);
        if (!net::limitPose(state, cfg_.boneReach)) { ++stats_.dropped; return; }
        proto::Encoder encoder;
        n = proto::writeState(copy, h, state, encoder);
        if (!n) { ++stats_.dropped; return; }
        out = copy;
    } else if (h.type == proto::kTypeAudio) {
        net::PlayerAudio sample;
        if (!proto::readAudio(payload, len, sample)) { ++stats_.dropped; return; }
    } else if (h.type == proto::kTypeHello) {
        std::string raw;
        uint32_t look = 0;
        uint8_t flags = 0;
        if (!proto::readHello(payload, len, raw, look, &flags)) { ++stats_.dropped; return; }
        const std::string name = cleanText(raw);
        if (!m->announced) {
            m->announced = true;
            m->name = name;
            log_("player " + hex(m->session) + " '" + name + "' joined from " + toString(from) + "; " +
                 std::to_string(members_.size()) + " connected");
            if (onEvent_) onEvent_(Event{Event::Joined, name, members_.size(), {}});
            if (!cfg_.motd.empty()) queueChat(cfg_.motd, &from);
        } else if (name != m->name) {
            if (logAllowed(*m, nowMs)) log_("player " + hex(m->session) + " '" + m->name + "' is now called '" + name + "'");
            m->name = name;
        }
        if (len > proto::kHelloFlagsOffset) { // only the server may say it hosts
            memcpy(copy, buf, n);
            copy[sizeof(h) + proto::kHelloFlagsOffset] =
                static_cast<char>(copy[sizeof(h) + proto::kHelloFlagsOffset] &
                                  ~(proto::kHelloHost | proto::kHelloServer | proto::kHelloMembersHard));
            out = copy;
        }
    } else if (h.type == proto::kTypeResult) {
        // A finished run: forwarded like any packet; reported once (it arrives up to 3 times).
        proto::Result r;
        if (!proto::readResult(payload, len, r)) { ++stats_.dropped; return; }
        bool seen = false;
        for (uint32_t x : resultSeen_) seen = seen || (x == (r.id ^ m->session));
        constexpr uint32_t kResultGapMs = 10000; // a run is longer than this; more often is flooding
        if (!seen && m->announced && (!m->resultAt || nowMs - m->resultAt >= kResultGapMs)) {
            m->resultAt = nowMs ? nowMs : 1;
            resultSeen_[resultSeenNext_++ % 32] = r.id ^ m->session;
            log_("run: " + proto::resultText(m->name, r));
            if (onEvent_) {
                Event e{Event::Result, m->name, members_.size(), r};
                onEvent_(e);
            }
        }
    } else if (h.type == proto::kTypeChat && cfg_.logChat) {
        uint32_t id = 0;
        std::string text;
        if (proto::readChat(payload, len, id, text)) {
            // Each message arrives up to 3 times (UDP may drop one): log it once.
            bool seen = false;
            for (uint32_t x : chatSeen_) seen = seen || (x == (id ^ m->session));
            if (!seen) {
                chatSeen_[chatSeenNext_++ % 32] = id ^ m->session;
                if (logAllowed(*m, nowMs)) log_("chat '" + m->name + "': " + text);
            }
        }
    }
    for (const auto& o : members_)
        if (o.addr != from) {
            send_(o.addr, out, n);
            ++stats_.relayed;
        }
    if (cfg_.mirror && h.type != proto::kTypeChat) {
        char mirror[proto::kMaxPacket];
        memcpy(mirror, out, n);
        proto::Header mh = h;
        mh.session = mirrorSession(h.session);
        memcpy(mirror, &mh, sizeof(mh));
        if (h.type == proto::kTypeState && len >= sizeof(float)) { // pos[0] is the first field
            float x = 0;
            memcpy(&x, mirror + sizeof(h), sizeof(x));
            x += 3.f;
            memcpy(mirror + sizeof(h), &x, sizeof(x));
        }
        send_(from, mirror, n);
    }
}

void Relay::challenge(const Addr& to, uint32_t nowMs) {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeChallenge, session_, ++seq_};
    send_(to, buf, proto::writeCookie(buf, h, proto::admissionCookie(cookieKey_, to.ip, to.port, nowMs / 60000)));
}

bool Relay::cookieValid(const Addr& from, uint64_t cookie, uint32_t nowMs) const {
    // This minute's cookie, or the last minute's (a challenge may have been answered just after the minute changed).
    const uint32_t minute = nowMs / 60000;
    if (cookie == proto::admissionCookie(cookieKey_, from.ip, from.port, minute)) return true;
    return minute > 0 && cookie == proto::admissionCookie(cookieKey_, from.ip, from.port, minute - 1);
}

void Relay::onAnswer(const Addr& from, uint32_t session, const char* payload, size_t len, uint32_t nowMs) {
    uint64_t cookie = 0;
    if (!proto::readCookie(payload, len, cookie) || find(from) || !cookieValid(from, cookie, nowMs)) {
        ++stats_.dropped;
        return;
    }
    admit(from, session, nowMs);
}

// Lets a player in once it has answered the challenge (its address is proven). The same checks as before.
void Relay::admit(const Addr& from, uint32_t session, uint32_t nowMs) {
    if (sessionTaken(session, from)) { ++stats_.dropped; return; }
    if (members_.size() >= cfg_.maxPlayers) {
        ++stats_.dropped;
        if (!fullWarned_ || nowMs - fullWarnAt_ > 10000) {
            fullWarned_ = true;
            fullWarnAt_ = nowMs;
            log_("server full (" + std::to_string(members_.size()) + " players); not admitting " + toString(from));
        }
        notify(from, "This server is full (" + std::to_string(members_.size()) + "/" + std::to_string(cfg_.maxPlayers) +
                         "). Try again later.", nowMs);
        return;
    }
    size_t sameIp = 0;
    for (const auto& o : members_) sameIp += o.addr.ip == from.ip;
    if (sameIp >= cfg_.maxPerIp) {
        ++stats_.dropped;
        notify(from, "Too many players from your address on this server.", nowMs);
        return;
    }
    Member nm;
    nm.addr = from;
    nm.session = session;
    nm.joinedAt = nm.lastSeen = nm.refillAt = nowMs;
    nm.tokens = cfg_.burst;
    members_.push_back(nm);
    haveHello_ = false; // greet the newcomer right away (it learns the session and our name)
}

void Relay::sendHello() {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeHello, session_, ++seq_};
    const size_t n = proto::writeHello(buf, h, cfg_.name.c_str(), 0, proto::kHelloHost | proto::kHelloServer);
    for (const auto& m : members_) send_(m.addr, buf, n);
}

void Relay::sendHelloTo(const Addr& to) {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeHello, session_, ++seq_};
    send_(to, buf, proto::writeHello(buf, h, cfg_.name.c_str(), 0, proto::kHelloHost | proto::kHelloServer));
}

void Relay::notify(const Addr& to, const std::string& text, uint32_t nowMs) {
    for (size_t i = 0; i < notices_.size();) {
        if (nowMs - notices_[i].at >= 15000) { notices_.erase(notices_.begin() + static_cast<long>(i)); continue; }
        if (notices_[i].to == to) return; // told recently
        ++i;
    }
    if (notices_.size() >= 256) return; // many strangers at once: do not let them make us send
    notices_.push_back({to, nowMs});
    // Our HELLO first, so the client knows who is talking; then the line twice (UDP may drop one).
    sendHelloTo(to);
    char buf[proto::kMaxPacket];
    const uint32_t id = ++chatId_;
    for (int k = 0; k < 2; ++k) {
        const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeChat, session_, ++seq_};
        send_(to, buf, proto::writeChat(buf, h, id, proto::cleanChatText(text)));
    }
}

void Relay::queueChat(const std::string& raw, const Addr* only) {
    const std::string text = proto::cleanChatText(raw);
    if (text.empty()) return;
    chat_.push_back({++chatId_, text, 3, only != nullptr, only ? *only : Addr{}});
}

void Relay::tick(uint32_t nowMs) {
    for (size_t i = 0; i < reserved_.size();) {
        if (static_cast<int32_t>(nowMs - reserved_[i].until) >= 0) reserved_.erase(reserved_.begin() + static_cast<long>(i));
        else ++i;
    }
    for (size_t i = 0; i < members_.size();) {
        if (nowMs - members_[i].lastSeen > cfg_.timeoutMs) {
            reserved_.push_back({members_[i].session, members_[i].addr, nowMs + cfg_.sessionGraceMs});
            drop(i, "timed out");
            continue;
        }
        ++i;
    }
    if (!haveHello_ || nowMs - helloAt_ >= cfg_.helloEveryMs) {
        haveHello_ = true;
        helloAt_ = nowMs;
        sendHello();
    }
    // Server chat: each message in 3 consecutive ticks.
    for (auto& c : chat_) {
        char buf[proto::kMaxPacket];
        const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeChat, session_, ++seq_};
        const size_t n = proto::writeChat(buf, h, c.id, c.text);
        for (const auto& m : members_)
            if (!c.targeted || m.addr == c.to) send_(m.addr, buf, n);
        --c.left;
    }
    while (!chat_.empty() && chat_.front().left <= 0) chat_.pop_front();
}

size_t Relay::matchPlayers(const std::string& who, size_t* first) const {
    size_t n = 0;
    for (size_t i = 0; i < members_.size(); ++i)
        if (hex(members_[i].session) == who || members_[i].name == who) {
            if (n == 0 && first) *first = i;
            ++n;
        }
    return n;
}

bool Relay::kick(const std::string& who, uint32_t nowMs) {
    size_t i = 0;
    if (matchPlayers(who, &i) != 1) return false; // nobody, or a name several players share
    const Member& m = members_[i];
    blocks_.push_back({m.addr.ip, 0, nowMs + cfg_.kickBlockMs}); // the address, not just this port
    drop(i, "kicked");
    return true;
}

void Relay::say(const std::string& raw) {
    const std::string text = proto::cleanChatText(raw);
    if (text.empty()) return;
    queueChat(text, nullptr);
    log_("chat (server): " + text);
}

uint32_t Relay::ban(const std::string& who) {
    uint32_t ip = 0;
    if (!parseIp(who, ip)) {
        size_t i = 0;
        if (matchPlayers(who, &i) != 1) return 0; // nobody, or a name several players share
        ip = members_[i].addr.ip;
    }
    if (!ip) return 0;
    std::set<uint32_t> next = bans_;
    next.insert(ip);
    setBans(std::move(next));
    log_("banned " + ipToString(ip));
    return ip;
}

bool Relay::unban(const std::string& text) {
    uint32_t ip = 0;
    if (!parseIp(text, ip) || !bans_.erase(ip)) return false;
    log_("unbanned " + ipToString(ip));
    return true;
}

void Relay::setBans(std::set<uint32_t> ips) {
    bans_ = std::move(ips);
    for (size_t i = 0; i < members_.size();) {
        if (bans_.count(members_[i].addr.ip)) { drop(i, "banned"); continue; }
        ++i;
    }
}

void Relay::reset() {
    while (!members_.empty()) drop(0, "server reset");
    reserved_.clear();
    notices_.clear();
    chat_.clear();
    fullWarned_ = false;
    fullWarnAt_ = 0;
    stats_ = Stats{};
}

void Relay::reconfigure(Config cfg) { cfg_ = std::move(cfg); }

} // namespace mecmp::server
