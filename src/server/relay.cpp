#include "server/relay.h"

#include <cstdio>
#include <cstring>
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
      chatId_(serverSession * 2654435761u) {}

Member* Relay::find(const Addr& a) {
    for (auto& m : members_)
        if (m.addr == a) return &m;
    return nullptr;
}

bool Relay::sessionTaken(uint32_t session, const Addr& except) const {
    for (const auto& m : members_)
        if (m.session == session && m.addr != except) return true;
    return session == session_;
}

bool Relay::blocked(const Addr& a, uint32_t nowMs) {
    for (size_t i = 0; i < blocks_.size();) {
        if (static_cast<int32_t>(nowMs - blocks_[i].until) >= 0) {
            blocks_.erase(blocks_.begin() + static_cast<long>(i));
            continue;
        }
        if (blocks_[i].ip == a.ip && blocks_[i].port == a.port) return true;
        ++i;
    }
    return false;
}

void Relay::drop(size_t index, const char* why) {
    const Member& m = members_[index];
    log_("player " + hex(m.session) + " '" + m.name + "' (" + toString(m.addr) + ") left: " + why + "; " +
         std::to_string(members_.size() - 1) + " connected");
    members_.erase(members_.begin() + static_cast<long>(index));
}

void Relay::onPacket(const Addr& from, const char* buf, size_t n, uint32_t nowMs) {
    proto::Header h;
    if (n < sizeof(h) || n > proto::kMaxPacket) { ++stats_.dropped; return; }
    memcpy(&h, buf, sizeof(h));
    if (h.magic != proto::kMagic || h.protocol != proto::kProtocol || !proto::knownType(h.type)) {
        ++stats_.dropped;
        return;
    }
    if (blocked(from, nowMs)) { ++stats_.dropped; return; }
    Member* m = find(from);
    if (!m) {
        if (sessionTaken(h.session, from)) { ++stats_.dropped; return; }
        if (members_.size() >= cfg_.maxPlayers) {
            ++stats_.dropped;
            if (!fullWarned_ || nowMs - fullWarnAt_ > 10000) {
                fullWarned_ = true;
                fullWarnAt_ = nowMs;
                log_("server full (" + std::to_string(members_.size()) + " players); not admitting " + toString(from));
            }
            return;
        }
        Member nm;
        nm.addr = from;
        nm.session = h.session;
        nm.joinedAt = nm.lastSeen = nm.refillAt = nowMs;
        nm.tokens = cfg_.burst;
        members_.push_back(nm);
        m = &members_.back();
        haveHello_ = false; // greet the newcomer right away (it learns the session and our name)
    } else if (h.session != m->session) {
        // Same address, new id: the game was restarted. Only a HELLO may change it, and only to an id
        // nobody else uses.
        if (h.type != proto::kTypeHello || sessionTaken(h.session, from)) { ++stats_.dropped; ++m->dropped; return; }
        log_("player " + hex(m->session) + " '" + m->name + "' is now " + hex(h.session) + " (restarted)");
        m->session = h.session;
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
    if (h.type == proto::kTypeHello) {
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
        } else if (name != m->name) {
            log_("player " + hex(m->session) + " '" + m->name + "' is now called '" + name + "'");
            m->name = name;
        }
        if (len > proto::kHelloFlagsOffset) { // only the server may say it hosts
            memcpy(copy, buf, n);
            copy[sizeof(h) + proto::kHelloFlagsOffset] =
                static_cast<char>(copy[sizeof(h) + proto::kHelloFlagsOffset] & ~(proto::kHelloHost | proto::kHelloServer));
            out = copy;
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
                log_("chat '" + m->name + "': " + text);
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

void Relay::sendHello() {
    char buf[proto::kMaxPacket];
    const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeHello, session_, ++seq_};
    const size_t n = proto::writeHello(buf, h, cfg_.name.c_str(), 0, proto::kHelloHost | proto::kHelloServer);
    for (const auto& m : members_) send_(m.addr, buf, n);
}

void Relay::tick(uint32_t nowMs) {
    for (size_t i = 0; i < members_.size();) {
        if (nowMs - members_[i].lastSeen > cfg_.timeoutMs) {
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
        for (const auto& m : members_) send_(m.addr, buf, n);
        --c.left;
    }
    while (!chat_.empty() && chat_.front().left <= 0) chat_.pop_front();
}

bool Relay::kick(const std::string& who, uint32_t nowMs) {
    for (size_t i = 0; i < members_.size(); ++i) {
        const Member& m = members_[i];
        if (hex(m.session) == who || m.name == who) {
            blocks_.push_back({m.addr.ip, m.addr.port, nowMs + cfg_.kickBlockMs});
            drop(i, "kicked");
            return true;
        }
    }
    return false;
}

void Relay::say(const std::string& raw) {
    std::string text = proto::cleanChatText(raw);
    if (text.empty()) return;
    chat_.push_back({++chatId_, text, 3});
    log_("chat (server): " + text);
}

} // namespace mecmp::server
