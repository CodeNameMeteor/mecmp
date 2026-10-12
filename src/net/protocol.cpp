#include "net/protocol.h"

#include <cmath>
#include <cstring>

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
        float v = (q[i] * sign * kSqrt2 + 1.f) * 0.5f * 1023.f + 0.5f; // [-1/sqrt2, 1/sqrt2] -> 0..1023
        const int iv = v < 0 ? 0 : (v > 1023 ? 1023 : static_cast<int>(v));
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
        q[i] = (static_cast<float>((w >> shift) & 1023) / 1023.f * 2.f - 1.f) / kSqrt2;
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
bool sanePlacement(const PlayerState& s) {
    for (float v : s.pos)
        if (!std::isfinite(v) || std::fabs(v) > kMaxCoord) return false;
    return std::isfinite(s.yaw);
}
} // namespace

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
