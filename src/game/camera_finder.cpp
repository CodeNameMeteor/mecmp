#include "game/camera_finder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"

namespace mecmp::game::camera_finder {
namespace {

constexpr size_t kViewProjOffset = 0x4a0; // in the render view (DebugRenderer_drawPrimitiveBatches)
constexpr uintptr_t kImageBase = 0x140000000;

struct Vec3 { float x, y, z; };

// The camera's view-projection matrix. Seen in the first runs: Frostbite's reversed infinite
// projection, so the clip-z row/column is (0, 0, 0, near) with near = 0.06, and the clip-w one is the
// unit view direction plus an offset (clip.w ~ 0 at the eye). `transposed` = stored column-major
// (clip = M * p, rows as stored); otherwise row vectors (clip = p * M).
bool looksLikeViewProj(const float* m, const Vec3& eye, bool transposed, float& wAtEye) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e5f) return false;
    // at(k, i): coefficient of input i (x, y, z, 1) in clip component k (x, y, z, w)
    auto at = [&](int k, int i) { return transposed ? m[k * 4 + i] : m[i * 4 + k]; };
    // No requirement on the clip-z part: the render view may store its matrix with another depth mapping
    // than the shader copies (those use reversed infinite depth: z row 0,0,0,0.06).
    const float fx = at(3, 0), fy = at(3, 1), fz = at(3, 2);
    const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 0.98f || len > 1.02f) return false;
    const float sx = std::sqrt(at(0, 0) * at(0, 0) + at(0, 1) * at(0, 1) + at(0, 2) * at(0, 2));
    const float sy = std::sqrt(at(1, 0) * at(1, 0) + at(1, 1) * at(1, 1) + at(1, 2) * at(1, 2));
    if (sx < 0.1f || sx > 10.f || sy < 0.1f || sy > 10.f) return false;
    wAtEye = eye.x * fx + eye.y * fy + eye.z * fz + at(3, 3);
    return std::fabs(wAtEye) < 2.5f;
}

template <class Fn> void forEachHeapChunk(std::vector<uint8_t>& buf, Fn fn) {
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t addr = 0x10000; VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_MAPPED) || mbi.Protect != PAGE_READWRITE)
            continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t own = reinterpret_cast<uintptr_t>(buf.data());
        if (own >= base && own < base + mbi.RegionSize) continue; // our own read buffer holds stale copies
        for (size_t off = 0; off < mbi.RegionSize; off += buf.size()) {
            const size_t n = std::min(buf.size(), static_cast<size_t>(mbi.RegionSize - off));
            if (mem::read(base + off, buf.data(), n)) fn(base + off, buf.data(), n);
        }
    }
}

// Writable sections of the game image (globals).
template <class Fn> void forEachImageGlobal(Fn fn) {
    const auto* img = reinterpret_cast<const uint8_t*>(mem::moduleBase());
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uintptr_t start = reinterpret_cast<uintptr_t>(img) + sec->VirtualAddress;
        std::vector<uintptr_t> q(sec->Misc.VirtualSize / 8);
        for (size_t k = 0; k < q.size(); k += 4096) {
            const size_t n = std::min<size_t>(4096, q.size() - k);
            if (!mem::read(start + k * 8, &q[k], n * 8)) memset(&q[k], 0, n * 8);
        }
        for (size_t k = 0; k < q.size(); ++k) fn(start + k * 8, q[k]);
    }
}

uintptr_t toVa(uintptr_t a) { return a - mem::moduleBase() + kImageBase; }

struct Candidate { uintptr_t addr; bool transposed; float w; float m[16]; };
struct Holder { uintptr_t at; uintptr_t target; size_t cand; }; // *at = target, a pointer into candidate's object
} // namespace

void run() {
    void* player = level::playerCharacter();
    LinearTransform xf;
    if (!player || !readEntityTransform(player, xf)) {
        MLOG("camera: load into a level first");
        return;
    }
    const Vec3 eye{xf.trans[0], xf.trans[1] + 1.7f, xf.trans[2]}; // Faith's eyes, roughly
    MLOG("camera: looking for the view-projection matrix (eye about %.1f %.1f %.1f); keep the camera moving a little",
         eye.x, eye.y, eye.z);
    const DWORD start = GetTickCount();
    std::vector<uint8_t> buf(4 << 20);
    constexpr size_t kMaxCands = 32768;

    // 1) Every matrix with the camera's projection signature, in the heap and in the game's globals.
    std::vector<Candidate> cands;
    cands.reserve(kMaxCands);
    const uintptr_t candsAt = reinterpret_cast<uintptr_t>(cands.data()), candsEnd = candsAt + kMaxCands * sizeof(Candidate);
    auto scan = [&](uintptr_t base, const uint8_t* p, size_t n) {
        for (size_t o = 0; o + 64 <= n && cands.size() < kMaxCands; o += 16) {
            if (base + o >= candsAt && base + o < candsEnd) continue; // our own copies
            const auto* m = reinterpret_cast<const float*>(p + o);
            float w = 0;
            for (bool t : {false, true})
                if (looksLikeViewProj(m, eye, t, w)) {
                    Candidate c{base + o, t, w, {}};
                    memcpy(c.m, m, 64);
                    cands.push_back(c);
                    break;
                }
        }
    };
    forEachHeapChunk(buf, scan);
    {
        const auto* img = reinterpret_cast<const uint8_t*>(mem::moduleBase());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img + reinterpret_cast<const IMAGE_DOS_HEADER*>(img)->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            const uintptr_t a = reinterpret_cast<uintptr_t>(img) + sec->VirtualAddress;
            for (size_t off = 0; off < sec->Misc.VirtualSize; off += buf.size()) {
                const size_t n = std::min<size_t>(buf.size(), sec->Misc.VirtualSize - off);
                if (mem::read(a + off, buf.data(), n)) scan(a + off, buf.data(), n);
            }
        }
    }

    // 2) Keep the ones that still have the signature 0.3 s and 0.6 s later (scratch copies get reused).
    std::vector<bool> stable(cands.size(), true);
    std::vector<float> moved(cands.size(), 0.f);
    for (int pass = 0; pass < 2; ++pass) {
        Sleep(300);
        for (size_t i = 0; i < cands.size(); ++i) {
            float now[16], w = 0;
            if (!stable[i] || !mem::read(cands[i].addr, now, 64) || !looksLikeViewProj(now, eye, cands[i].transposed, w)) {
                stable[i] = false;
                continue;
            }
            for (int k = 0; k < 16; ++k) moved[i] = std::max(moved[i], std::fabs(now[k] - cands[i].m[k]));
        }
    }
    size_t nStable = 0;
    for (bool b : stable) nStable += b;
    MLOG("camera: %llu matrices with the projection signature, %llu still valid after 0.6 s",
         static_cast<unsigned long long>(cands.size()), static_cast<unsigned long long>(nStable));

    // 3) Who points at the object around each stable one: globals directly, or through one heap object.
    constexpr uintptr_t kObjBefore = 0x2000;
    std::vector<size_t> order; // stable candidates sorted by address, for lookups
    for (size_t i = 0; i < cands.size(); ++i)
        if (stable[i]) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cands[a].addr < cands[b].addr; });
    auto candFor = [&](uintptr_t p) -> long { // first stable candidate at p .. p + kObjBefore
        auto it = std::lower_bound(order.begin(), order.end(), p, [&](size_t i, uintptr_t x) { return cands[i].addr < x; });
        return it != order.end() && cands[*it].addr - p <= kObjBefore ? static_cast<long>(*it) : -1;
    };
    std::vector<int> refs(cands.size(), 0);
    int direct = 0;
    forEachImageGlobal([&](uintptr_t at, uintptr_t v) {
        const long c = v ? candFor(v) : -1;
        if (c < 0) return;
        ++refs[c];
        if (direct++ < 40)
            MLOG("camera: global 0x%llx -> object, matrix [%ld] at +0x%llx", static_cast<unsigned long long>(toVa(at)), c,
                 static_cast<unsigned long long>(cands[c].addr - v));
    });
    std::vector<Holder> holders;
    forEachHeapChunk(buf, [&](uintptr_t base, const uint8_t* p, size_t n) {
        const auto* q = reinterpret_cast<const uintptr_t*>(p);
        for (size_t i = 0; i < n / 8 && holders.size() < 200000; ++i) {
            const long c = q[i] > 0x10000 ? candFor(q[i]) : -1;
            if (c >= 0) holders.push_back({base + i * 8, q[i], static_cast<size_t>(c)});
        }
    });
    std::sort(holders.begin(), holders.end(), [](const Holder& a, const Holder& b) { return a.at < b.at; });
    int chains = 0;
    constexpr uintptr_t kHolderBefore = 0x1000;
    forEachImageGlobal([&](uintptr_t at, uintptr_t v) {
        if (!v) return;
        auto it = std::lower_bound(holders.begin(), holders.end(), v, [](const Holder& h, uintptr_t x) { return h.at < x; });
        for (; it != holders.end() && it->at - v <= kHolderBefore; ++it) {
            ++refs[it->cand];
            if (chains++ < 80)
                MLOG("camera: global 0x%llx -> +0x%llx -> object, matrix [%llu] at +0x%llx",
                     static_cast<unsigned long long>(toVa(at)), static_cast<unsigned long long>(it->at - v),
                     static_cast<unsigned long long>(it->cand), static_cast<unsigned long long>(cands[it->cand].addr - it->target));
        }
    });

    // 4) The stable candidates (referenced ones first), with their values.
    int shown = 0;
    for (int pass = 0; pass < 1; ++pass)
        for (size_t i : order) {
            if (refs[i] == 0 || shown >= 60) continue;
            ++shown;
            const Candidate& c = cands[i];
            const bool inImage = c.addr >= mem::moduleBase() && c.addr < mem::moduleBase() + 0x727b000;
            MLOG("camera: [%llu] %p%s %s refs=%d moved=%.3f w(eye)=%.2f | %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.2f %.2f %.2f %.2f",
                 static_cast<unsigned long long>(i), reinterpret_cast<void*>(c.addr), inImage ? " (game global)" : "",
                 c.transposed ? "col" : "row", refs[i], moved[i], c.w, c.m[0], c.m[1], c.m[2], c.m[3], c.m[4], c.m[5], c.m[6],
                 c.m[7], c.m[8], c.m[9], c.m[10], c.m[11], c.m[12], c.m[13], c.m[14], c.m[15]);
        }
    MLOG("camera: done in %.1f s (%d direct, %d through one object). The render view keeps its matrix at +0x%llx.",
         (GetTickCount() - start) / 1000.0, direct, chains, static_cast<unsigned long long>(kViewProjOffset));
}

} // namespace mecmp::game::camera_finder
