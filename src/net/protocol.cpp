#include "net/protocol.h"

#include <cmath>
#include <cctype>
#include <cstdio>
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
    const std::string label = cleanPingLabel(pg.label);
    put(p, static_cast<uint8_t>(label.size()));
    memcpy(p, label.data(), label.size());
    p += label.size();
    put(p, pg.life);
    put(p, pg.accent);
    put(p, pg.index);
    return static_cast<size_t>(p - buf);
}

bool readPing(const char* payload, size_t n, Ping& out) {
    const char* p = payload;
    const char* end = payload + n;
    Ping pg;
    if (!get(p, end, pg.id) || !get(p, end, pg.pos) || !sanePosition(pg.pos)) return false;
    // Optional tail (older senders stop early); the defaults stay otherwise. A damaged label is dropped
    // with everything after it (where that starts is not known then); the marker itself is still good.
    bool haveIndex = false;
    if (get(p, end, pg.flags) && get(p, end, pg.rgb)) {
        uint8_t len = 0;
        if (get(p, end, len) && len <= kMaxPingLabelBytes && static_cast<size_t>(end - p) >= len) {
            pg.label = cleanPingLabel(std::string(p, len));
            p += len;
            uint8_t accent[3];
            if (get(p, end, pg.life) && get(p, end, accent)) {
                memcpy(pg.accent, accent, sizeof(accent));
                haveIndex = get(p, end, pg.index);
            }
        }
    }
    const bool clear = (pg.flags & kPingClear) != 0;
    if (!haveIndex || pg.index >= kPingCells) pg.index = clear ? kPingAllCells : 0;
    out = pg;
    return true;
}

bool samePingMarker(const PingCellView& cell, uint8_t glyph, const std::string& label, const float pos[3]) {
    if (!cell.live || cell.glyph != glyph || cell.label != label) return false;
    float d2 = 0.f;
    for (int i = 0; i < 3; ++i) d2 += (cell.pos[i] - pos[i]) * (cell.pos[i] - pos[i]);
    return d2 <= kPingMergeMetres * kPingMergeMetres;
}

int choosePingCell(const PingCellView cells[kPingCells], int limit, uint8_t glyph, const std::string& label,
                   const float pos[3]) {
    limit = limit < 1 ? 1 : (limit > kPingCells ? kPingCells : limit);
    int same = -1, free = -1, oldest = -1, live = 0;
    float sameD2 = 0.f;
    for (int i = 0; i < kPingCells; ++i) {
        const PingCellView& c = cells[i];
        if (!c.live) {
            if (free < 0) free = i;
            continue;
        }
        ++live;
        if (oldest < 0 || c.order < cells[oldest].order) oldest = i;
        if (!samePingMarker(c, glyph, label, pos)) continue;
        float d2 = 0.f;
        for (int k = 0; k < 3; ++k) d2 += (c.pos[k] - pos[k]) * (c.pos[k] - pos[k]);
        if (same < 0 || d2 < sameD2) { same = i; sameD2 = d2; }
    }
    if (same >= 0) return same;
    if (live < limit && free >= 0) return free;
    return oldest >= 0 ? oldest : 0;
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

namespace {
// No control characters, no leading/trailing spaces, cut at `maxBytes` without splitting a character.
std::string cleanText(const std::string& raw, size_t maxBytes) {
    std::string s;
    for (unsigned char c : raw)
        if (c >= 0x20 && c != 0x7f) s.push_back(static_cast<char>(c));
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    s = s.substr(a, s.find_last_not_of(' ') - a + 1);
    s.resize(utf8PrefixLength(s, maxBytes));
    return s;
}
} // namespace

std::string cleanChatText(const std::string& raw) { return cleanText(raw, kMaxChatBytes); }
std::string cleanPingLabel(const std::string& raw) { return cleanText(raw, kMaxPingLabelBytes); }

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

size_t writeReact(char* buf, const Header& h, const React& r) {
    char* p = buf;
    put(p, h);
    put(p, r.id);
    put(p, r.target);
    put(p, r.cell);
    for (const std::string* field : {&r.title, &r.text}) {
        const std::string t = cleanPingLabel(*field);
        put(p, static_cast<uint8_t>(t.size()));
        memcpy(p, t.data(), t.size());
        p += t.size();
    }
    return static_cast<size_t>(p - buf);
}

bool readReact(const char* payload, size_t n, React& out) {
    const char* p = payload;
    const char* end = payload + n;
    React r;
    if (!get(p, end, r.id) || !get(p, end, r.target) || !get(p, end, r.cell) || r.cell >= kPingCells) return false;
    std::string* fields[2] = {&r.title, &r.text};
    for (std::string* field : fields) {
        uint8_t len = 0;
        if (!get(p, end, len) || len > kMaxPingLabelBytes || static_cast<size_t>(end - p) < len) return false;
        *field = cleanPingLabel(std::string(p, len));
        p += len;
    }
    if (r.text.empty()) return false;
    out = r;
    return true;
}

size_t writeSummon(char* buf, const Header& h, const Summon& s) {
    char* p = buf;
    put(p, h);
    put(p, s.id);
    for (int i = 0; i < 3; ++i) put(p, s.pos[i]);
    put(p, s.yaw);
    put(p, static_cast<uint8_t>(s.flags & kSummonKnownFlags));
    const uint8_t n = static_cast<uint8_t>(s.targetCount > kMaxSummonTargets ? kMaxSummonTargets : s.targetCount);
    put(p, n);
    for (int i = 0; i < n; ++i) put(p, s.targets[i]);
    return static_cast<size_t>(p - buf);
}

bool readSummon(const char* payload, size_t n, Summon& out) {
    const char* p = payload;
    const char* end = payload + n;
    Summon s;
    uint8_t flags = 0;
    if (!get(p, end, s.id)) return false;
    for (int i = 0; i < 3; ++i)
        if (!get(p, end, s.pos[i])) return false;
    if (!get(p, end, s.yaw) || !get(p, end, flags) || !get(p, end, s.targetCount)) return false;
    if (s.targetCount > kMaxSummonTargets || !sanePosition(s.pos) || !std::isfinite(s.yaw)) return false;
    for (int i = 0; i < s.targetCount; ++i)
        if (!get(p, end, s.targets[i])) return false;
    s.flags = static_cast<uint8_t>(flags & kSummonKnownFlags);
    out = s;
    return true;
}

bool summonCalls(const Summon& s, uint32_t session) {
    if (s.targetCount == 0) return true;
    for (int i = 0; i < s.targetCount && i < kMaxSummonTargets; ++i)
        if (s.targets[i] == session) return true;
    return false;
}

void applyHostPolicy(Summon& s, bool senderIsHost, bool membersMayHard) {
    s.flags = static_cast<uint8_t>(s.flags & ~kSummonVouched); // never what a sender wrote
    if (senderIsHost || (s.flags & kSummonHard) == 0) return;   // the host needs no vouching
    if (membersMayHard) s.flags = static_cast<uint8_t>(s.flags | kSummonVouched);
    else s.flags = static_cast<uint8_t>(s.flags & ~kSummonHard);
}

bool patchSummonFlags(char* packet, size_t n, uint8_t flags) {
    constexpr size_t kFlagsAt = sizeof(Header) + sizeof(uint32_t) + 3 * sizeof(float) + sizeof(float);
    if (n <= kFlagsAt) return false;
    packet[kFlagsAt] = static_cast<char>(flags & kSummonKnownFlags);
    return true;
}

size_t writeSummonReply(char* buf, const Header& h, const SummonReply& r) {
    char* p = buf;
    put(p, h);
    put(p, r.id);
    put(p, r.target);
    put(p, r.result);
    return static_cast<size_t>(p - buf);
}

bool readSummonReply(const char* payload, size_t n, SummonReply& out) {
    const char* p = payload;
    const char* end = payload + n;
    SummonReply r;
    if (!get(p, end, r.id) || !get(p, end, r.target) || !get(p, end, r.result) || r.result > kSummonBusy) return false;
    out = r;
    return true;
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
    // The name comes from whatever server answers: shown as chat and in the menu, so clean it like a player's.
    out.name = cleanText(std::string(payload + 4, len), kMaxNameBytes);
    return true;
}

size_t writeHello(char* buf, const Header& h, const char* name, uint32_t look, uint8_t flags, uint8_t wear,
                  const OnlineProfile* profile) {
    char* p = buf;
    put(p, h);
    const std::string full = name ? name : "";
    char n[kNameLen] = {};
    memcpy(n, full.data(), utf8PrefixLength(full, kMaxNameBytes));
    memcpy(p, n, kNameLen);
    p += kNameLen;
    put(p, look);
    put(p, flags);
    put(p, wear);
    if (profile && profile->id) {
        put(p, profile->id);
        char pn[kProfileLen] = {};
        for (size_t i = 0; i + 1 < kProfileLen && profile->name[i]; ++i) pn[i] = profile->name[i];
        memcpy(p, pn, kProfileLen);
        p += kProfileLen;
    }
    return static_cast<size_t>(p - buf);
}

bool readHello(const char* payload, size_t n, std::string& name, uint32_t& look, uint8_t* flags, uint8_t* wear,
               OnlineProfile* profile) {
    if (n == 0) return false;
    const size_t nameBytes = n < kNameLen ? n : kNameLen;
    size_t len = 0;
    while (len < nameBytes && payload[len]) ++len;
    name.assign(payload, len);
    look = 0;
    if (n >= kNameLen + sizeof(uint32_t)) memcpy(&look, payload + kNameLen, sizeof(look));
    if (flags) *flags = n >= kNameLen + sizeof(uint32_t) + 1 ? static_cast<uint8_t>(payload[kNameLen + sizeof(uint32_t)]) : 0;
    if (wear) *wear = n >= kNameLen + sizeof(uint32_t) + 2 ? static_cast<uint8_t>(payload[kNameLen + sizeof(uint32_t) + 1]) : 0;
    if (profile) {
        *profile = OnlineProfile{};
        const size_t at = kNameLen + sizeof(uint32_t) + 2;
        if (n >= at + sizeof(uint64_t) + kProfileLen) {
            memcpy(&profile->id, payload + at, sizeof(uint64_t));
            // Printable ASCII only (EA profile names), always NUL terminated.
            for (size_t i = 0; i + 1 < kProfileLen; ++i) {
                const char c = payload[at + sizeof(uint64_t) + i];
                if (c < 0x20 || c > 0x7e) break;
                profile->name[i] = c;
            }
            if (!profile->name[0]) profile->id = 0;
        }
    }
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


// --- RESULT ------------------------------------------------------------------------------------------

namespace {
std::string cleanResultTitle(const std::string& raw) {
    const std::string t = cleanChatText(raw);
    return t.substr(0, utf8PrefixLength(t, kMaxResultTitleBytes));
}
} // namespace

size_t writeResult(char* buf, const Header& h, const Result& r) {
    char* p = buf;
    put(p, h);
    put(p, r.id);
    put(p, r.contentId);
    put(p, r.kind);
    put(p, r.flags);
    put(p, r.value);
    put(p, r.delta);
    const std::string t = cleanResultTitle(r.title);
    put(p, static_cast<uint8_t>(t.size()));
    memcpy(p, t.data(), t.size());
    p += t.size();
    return static_cast<size_t>(p - buf);
}

bool readResult(const char* payload, size_t n, Result& out) {
    const char* p = payload;
    const char* end = payload + n;
    Result r;
    uint8_t len = 0;
    if (!get(p, end, r.id) || !get(p, end, r.contentId) || !get(p, end, r.kind) || !get(p, end, r.flags) ||
        !get(p, end, r.value) || !get(p, end, r.delta) || !get(p, end, len) || len > kMaxResultTitleBytes ||
        static_cast<size_t>(end - p) < len)
        return false;
    if (r.value < 0 || r.value > 24LL * 3600 * 1000) return false; // a day: not a run time
    r.title = cleanResultTitle(std::string(p, len));
    out = r;
    return true;
}

size_t writeActivity(char* buf, const Header& h, const Activity& a) {
    char* p = buf;
    put(p, h);
    put(p, a.kind);
    put(p, a.elapsedMs);
    const std::string t = a.kind == kActivityNone ? std::string() : cleanResultTitle(a.title);
    put(p, static_cast<uint8_t>(t.size()));
    memcpy(p, t.data(), t.size());
    p += t.size();
    return static_cast<size_t>(p - buf);
}

bool readActivity(const char* payload, size_t n, Activity& out) {
    const char* p = payload;
    const char* end = payload + n;
    Activity a;
    uint8_t len = 0;
    if (!get(p, end, a.kind) || !get(p, end, a.elapsedMs) || !get(p, end, len) || len > kMaxResultTitleBytes ||
        static_cast<size_t>(end - p) < len)
        return false;
    if (a.elapsedMs > 24u * 3600 * 1000) return false; // a day: not a run
    a.title = cleanResultTitle(std::string(p, len));
    out = a;
    return true;
}

const char* runKindName(uint8_t kind) {
    switch (kind) {
    case kRunDash: return "Dash";
    case kRunTimeTrial: return "Time trial";
    case kRunDelivery: return "Delivery";
    case kRunGridNode: return "GridNode";
    case kRunBillboard: return "Billboard hack";
    case kRunMetaGrid: return "MetaGrid";
    case kRunAssault: return "Assault";
    default: return "Run";
    }
}

std::string formatRunTime(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t cs = (ms + 5) / 10; // hundredths
    const int64_t h = cs / 360000, m = (cs / 6000) % 60, s = (cs / 100) % 60, c = cs % 100;
    char buf[32];
    if (h) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld", static_cast<long long>(h), static_cast<long long>(m),
                    static_cast<long long>(s), static_cast<long long>(c));
    else if (m) snprintf(buf, sizeof(buf), "%lld:%02lld.%02lld", static_cast<long long>(m), static_cast<long long>(s),
                         static_cast<long long>(c));
    else snprintf(buf, sizeof(buf), "%lld.%02lld", static_cast<long long>(s), static_cast<long long>(c));
    return buf;
}

std::string resultText(const std::string& player, const Result& r) {
    std::string kind = runKindName(r.kind);
    for (auto& c : kind) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (kind == "gridnode") kind = "GridNode";
    else if (kind == "metagrid") kind = "MetaGrid";
    // "the dash The View", or "a time trial" when it has no name.
    std::string s = (player.empty() ? std::string("Runner") : player) + " finished " +
                     (r.title.empty() ? std::string("a ") : std::string("the ")) + kind;
    if (!r.title.empty()) s += " " + r.title;
    s += " in " + formatRunTime(r.value);
    if (r.flags & kResultNewRecord) {
        s += " (new record";
        if ((r.flags & kResultHadRecord) && r.delta > 0) s += ", " + formatRunTime(r.delta) + " faster";
        s += ")";
    }
    return s;
}

// --- Admission cookies (see kTypeChallenge in protocol.h) -----------------------------------------------
namespace {
uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
} // namespace

uint64_t siphash24(const uint8_t key[16], const uint8_t* in, size_t len) {
    const uint64_t k0 = le64(key), k1 = le64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
        v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
    };
    const size_t whole = len - len % 8;
    for (size_t i = 0; i < whole; i += 8) {
        const uint64_t m = le64(in + i);
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    uint64_t last = static_cast<uint64_t>(len) << 56;
    for (size_t i = 0; i < len % 8; ++i) last |= static_cast<uint64_t>(in[whole + i]) << (8 * i);
    v3 ^= last;
    round();
    round();
    v0 ^= last;
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

uint64_t admissionCookie(const uint8_t key[16], uint32_t ip, uint16_t port, uint32_t minute) {
    uint8_t msg[10];
    for (int i = 0; i < 4; ++i) msg[i] = static_cast<uint8_t>(ip >> (24 - 8 * i));
    msg[4] = static_cast<uint8_t>(port >> 8);
    msg[5] = static_cast<uint8_t>(port);
    for (int i = 0; i < 4; ++i) msg[6 + i] = static_cast<uint8_t>(minute >> (24 - 8 * i));
    return siphash24(key, msg, sizeof(msg));
}

size_t writeCookie(char* buf, const Header& h, uint64_t cookie) {
    char* p = buf;
    put(p, h);
    put(p, cookie);
    return static_cast<size_t>(p - buf);
}

bool readCookie(const char* payload, size_t n, uint64_t& cookie) {
    if (n < kCookieLen) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload);
    cookie = le64(p);
    return true;
}

} // namespace mecmp::net::protocol
