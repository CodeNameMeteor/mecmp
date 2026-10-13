#include "net/protocol.h"

#include <cmath>
#include <cstring>
#include <string>

namespace mecmp::net::protocol {
namespace {
constexpr float kTransScale = 1024.f;
constexpr float kSqrt2 = 1.41421356f;

uint32_t encodeQuat(const float* q) {
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(q[i]) > std::fabs(q[big])) big = i;
    const float sign = q[big] < 0 ? -1.f : 1.f;
    uint32_t out = static_cast<uint32_t>(big) << 30;
    int shift = 20;
    for (int i = 0; i < 4; ++i) {
        if (i == big) continue;
        // Integer centre preserves exact zero in the world-space root rotation.
        float v = (q[i] * sign * kSqrt2 + 1.f) * 511.f + 0.5f;
        const int iv = v < 0 ? 0 : (v > 1022 ? 1022 : static_cast<int>(v));
        out |= static_cast<uint32_t>(iv) << shift;
        shift -= 10;
    }
    return out;
}

void decodeQuat(uint32_t w, float* q) {
    const int big = static_cast<int>(w >> 30);
    int shift = 20;
    float sum = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == big) continue;
        const uint32_t component = (w >> shift) & 1023;
        q[i] = (static_cast<float>(component > 1022 ? 1022 : component) / 1022.f * 2.f - 1.f) / kSqrt2;
        sum += q[i] * q[i];
        shift -= 10;
    }
    q[big] = std::sqrt(sum < 1.f ? 1.f - sum : 0.f);
}

int16_t quantT(float v) {
    float x = v * kTransScale;
    x = x < -32767.f ? -32767.f : (x > 32767.f ? 32767.f : x);
    return static_cast<int16_t>(std::lround(x));
}

template <class T> void put(char*& p, const T& v) { memcpy(p, &v, sizeof(T)); p += sizeof(T); }
template <class T> bool get(const char*& p, const char* end, T& v) {
    if (end - p < static_cast<ptrdiff_t>(sizeof(T))) return false;
    memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

// Positions are metres in the level's world; anything beyond this (or NaN/inf) is a broken or hostile packet.
constexpr float kMaxCoord = 100000.f;
bool sanePlacement(const PlayerState& s) { return sanePosition(s.pos) && std::isfinite(s.yaw); }
} // namespace

bool sanePosition(const float pos[3]) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i]) || std::fabs(pos[i]) > kMaxCoord) return false;
    return true;
}

size_t writePing(char* buf, const Header& h, const Ping& pg) {
    char* p = buf;
    put(p, h);
    put(p, pg.id);
    put(p, pg.pos);
    put(p, pg.flags);
    put(p, pg.rgb);
    return static_cast<size_t>(p - buf);
}

bool readPing(const char* payload, size_t n, Ping& out) {
    const char* p = payload;
    const char* end = payload + n;
    Ping pg;
    if (!get(p, end, pg.id) || !get(p, end, pg.pos) || !sanePosition(pg.pos)) return false;
    if (get(p, end, pg.flags)) get(p, end, pg.rgb); // optional tail; the defaults stay otherwise
    out = pg;
    return true;
}

bool parseRgb(const std::string& hex, uint8_t rgb[3]) {
    const size_t at = !hex.empty() && hex[0] == '#' ? 1 : 0;
    if (hex.size() - at != 6) return false;
    uint8_t v[3] = {};
    for (size_t i = 0; i < 6; ++i) {
        const char c = hex[at + i];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0) return false;
        v[i / 2] = static_cast<uint8_t>(v[i / 2] * 16 + d);
    }
    memcpy(rgb, v, 3);
    return true;
}

std::string formatRgb(const uint8_t rgb[3]) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 3; ++i) {
        s.push_back(kHex[rgb[i] >> 4]);
        s.push_back(kHex[rgb[i] & 15]);
    }
    return s;
}

size_t writeState(char* buf, const Header& h, const PlayerState& s, Encoder& enc) {
    char* p = buf;
    put(p, h);
    put(p, s.pos);
    put(p, s.yaw);
    put(p, s.flags);
    put(p, s.timeMs);
    const uint8_t nb = static_cast<uint8_t>(s.boneCount <= kMaxPoseBones ? s.boneCount : 0);
    const bool full = nb && (nb != enc.lastFullCount || ++enc.sinceFull >= kFullEvery);
    int16_t qt[kMaxPoseBones][3];
    uint32_t mask = 0;
    for (uint8_t i = 0; i < nb; ++i) {
        for (int j = 0; j < 3; ++j) qt[i][j] = quantT(s.bones[i][j]);
        if (full || memcmp(qt[i], enc.lastFullT[i], sizeof(qt[i])) != 0) mask |= 1u << i;
    }
    if (full) {
        memcpy(enc.lastFullT, qt, sizeof(int16_t) * 3 * nb);
        enc.lastFullCount = nb;
        enc.sinceFull = 0;
    }
    put(p, nb);
    put(p, static_cast<uint8_t>(full ? 1 : 0));
    put(p, static_cast<uint16_t>(0));
    put(p, mask);
    for (uint8_t i = 0; i < nb; ++i) put(p, encodeQuat(&s.bones[i][4]));
    for (uint8_t i = 0; i < nb; ++i)
        if (mask & (1u << i)) {
            memcpy(p, qt[i], 6);
            p += 6;
        }
    return static_cast<size_t>(p - buf);
}

void alignRotations(PlayerState& s, const PlayerState& prev) {
    if (s.boneCount != prev.boneCount) return;
    for (uint32_t i = 0; i < s.boneCount && i < kMaxPoseBones; ++i) {
        float* q = &s.bones[i][4];
        const float* p = &prev.bones[i][4];
        if (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] * p[3] < 0.f)
            for (int j = 0; j < 4; ++j) q[j] = -q[j];
    }
}

size_t utf8PrefixLength(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s.size();
    size_t n = maxBytes;
    // Continuation bytes look like 10xxxxxx: back up to the first byte of the character that would be cut.
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

std::string cleanChatText(const std::string& raw) {
    std::string s;
    for (unsigned char c : raw)
        if (c >= 0x20 && c != 0x7f) s.push_back(static_cast<char>(c));
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    s = s.substr(a, s.find_last_not_of(' ') - a + 1);
    s.resize(utf8PrefixLength(s, kMaxChatBytes));
    return s;
}

size_t writeChat(char* buf, const Header& h, uint32_t id, const std::string& text) {
    const std::string t = cleanChatText(text);
    char* p = buf;
    put(p, h);
    put(p, id);
    put(p, static_cast<uint8_t>(t.size()));
    memcpy(p, t.data(), t.size());
    p += t.size();
    return static_cast<size_t>(p - buf);
}

bool readChat(const char* payload, size_t n, uint32_t& id, std::string& text) {
    const char* p = payload;
    const char* end = payload + n;
    uint8_t len = 0;
    if (!get(p, end, id) || !get(p, end, len) || len > kMaxChatBytes || end - p < len) return false;
    text = cleanChatText(std::string(p, len));
    return !text.empty();
}

size_t writeInfoRequest(char* buf, const Header& h) {
    memset(buf, 0, kInfoRequestSize);
    memcpy(buf, &h, sizeof(h));
    return kInfoRequestSize;
}

size_t writeInfoAnswer(char* buf, const Header& h, const ServerInfo& info) {
    char* p = buf;
    put(p, h);
    const uint8_t head[4] = {1, info.players, info.maxPlayers, 0};
    memcpy(p, head, sizeof(head));
    p += sizeof(head);
    char n[kNameLen] = {};
    memcpy(n, info.name.data(), utf8PrefixLength(info.name, kMaxNameBytes));
    memcpy(p, n, kNameLen);
    p += kNameLen;
    return static_cast<size_t>(p - buf);
}

bool isInfoRequest(const char* payload, size_t n) {
    return n + sizeof(Header) >= kInfoRequestSize && payload[0] == 0;
}

bool readInfoAnswer(const char* payload, size_t n, ServerInfo& out) {
    if (n < 4 + kNameLen || payload[0] != 1) return false;
    out.players = static_cast<uint8_t>(payload[1]);
    out.maxPlayers = static_cast<uint8_t>(payload[2]);
    size_t len = 0;
    while (len < kNameLen && payload[4 + len]) ++len;
    out.name.assign(payload + 4, len);
    return true;
}

size_t writeHello(char* buf, const Header& h, const char* name, uint32_t look, uint8_t flags) {
    char* p = buf;
    put(p, h);
    const std::string full = name ? name : "";
    char n[kNameLen] = {};
    memcpy(n, full.data(), utf8PrefixLength(full, kMaxNameBytes));
    memcpy(p, n, kNameLen);
    p += kNameLen;
    put(p, look);
    put(p, flags);
    return static_cast<size_t>(p - buf);
}

bool readHello(const char* payload, size_t n, std::string& name, uint32_t& look, uint8_t* flags) {
    if (n == 0) return false;
    const size_t nameBytes = n < kNameLen ? n : kNameLen;
    size_t len = 0;
    while (len < nameBytes && payload[len]) ++len;
    name.assign(payload, len);
    look = 0;
    if (n >= kNameLen + sizeof(uint32_t)) memcpy(&look, payload + kNameLen, sizeof(look));
    if (flags) *flags = n >= kNameLen + sizeof(uint32_t) + 1 ? static_cast<uint8_t>(payload[kNameLen + sizeof(uint32_t)]) : 0;
    return true;
}

bool readState(const char* payload, size_t n, const Decoder& dec, PlayerState& out, bool& full) {
    const char* p = payload;
    const char* end = payload + n;
    uint8_t nb = 0, fl = 0;
    uint16_t res = 0;
    uint32_t mask = 0;
    if (!get(p, end, out.pos) || !get(p, end, out.yaw) || !get(p, end, out.flags) || !get(p, end, out.timeMs) || !get(p, end, nb) ||
        !get(p, end, fl) || !get(p, end, res) || !get(p, end, mask))
        return false;
    if (!sanePlacement(out)) return false;
    full = (fl & 1) != 0;
    out.boneCount = 0;
    if (nb == 0) return true;
    if (nb > kMaxPoseBones) return false;
    if (!full && dec.baseCount != nb) return true; // no base yet: position only until a full packet
    // The rotations (4 bytes each) come first, the translations (6 bytes each, one per mask bit) after them.
    if (end - p < static_cast<ptrdiff_t>(nb) * 4) return false;
    const char* t = p + nb * 4;
    for (uint8_t i = 0; i < nb; ++i) {
        uint32_t w = 0;
        if (!get(p, end, w)) return false;
        decodeQuat(w, &out.bones[i][4]);
        if (mask & (1u << i)) {
            int16_t tv[3];
            if (end - t < 6) return false;
            memcpy(tv, t, 6);
            t += 6;
            for (int j = 0; j < 3; ++j) out.bones[i][j] = tv[j] / kTransScale;
        } else {
            for (int j = 0; j < 3; ++j) out.bones[i][j] = dec.baseT[i][j];
        }
        out.bones[i][3] = 1.f;
    }
    out.boneCount = nb;
    return true;
}

void commitBase(Decoder& dec, const PlayerState& s) {
    for (uint32_t i = 0; i < s.boneCount; ++i)
        for (int j = 0; j < 3; ++j) dec.baseT[i][j] = s.bones[i][j];
    dec.baseCount = s.boneCount;
}

} // namespace mecmp::net::protocol
