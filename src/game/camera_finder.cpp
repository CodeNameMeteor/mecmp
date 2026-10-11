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

// A view-projection matrix for a camera at (about) `eye`. Row vectors (Frostbite: clip = p * M),
// so clip.w = p . (M[0][3], M[1][3], M[2][3]) + M[3][3]; for a perspective camera that column is
// the unit view direction, and clip.w at the eye is ~0. `transposed` tests column vectors instead.
bool looksLikeViewProj(const float* m, const Vec3& eye, bool transposed, float& wAtEye) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e5f) return false;
    auto at = [&](int r, int c) { return transposed ? m[c * 4 + r] : m[r * 4 + c]; };
    const float fx = at(0, 3), fy = at(1, 3), fz = at(2, 3), fw = at(3, 3);
    const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 0.98f || len > 1.02f) return false;
    // x and y columns must scale (a projection, not a plain direction)
    const float sx = std::sqrt(at(0, 0) * at(0, 0) + at(1, 0) * at(1, 0) + at(2, 0) * at(2, 0));
    const float sy = std::sqrt(at(0, 1) * at(0, 1) + at(1, 1) * at(1, 1) + at(2, 1) * at(2, 1));
    if (sx < 0.2f || sx > 10.f || sy < 0.2f || sy > 10.f) return false;
    wAtEye = eye.x * fx + eye.y * fy + eye.z * fz + fw;
    return std::fabs(wAtEye) < 2.5f;
}

template <class Fn> void forEachHeapChunk(std::vector<uint8_t>& buf, Fn fn) {
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t addr = 0x10000; VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
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
    MLOG("camera: looking for the view-projection matrix (eye about %.1f %.1f %.1f); this takes a few seconds",
         eye.x, eye.y, eye.z);
    const DWORD start = GetTickCount();
    std::vector<uint8_t> buf(4 << 20);

    // 1) Matrices that fit a camera at the eye.
    std::vector<Candidate> cands;
    cands.reserve(64);
    const uintptr_t candsAt = reinterpret_cast<uintptr_t>(cands.data()), candsEnd = candsAt + 64 * sizeof(Candidate);
    forEachHeapChunk(buf, [&](uintptr_t base, const uint8_t* p, size_t n) {
        for (size_t o = 0; o + 64 <= n && cands.size() < 64; o += 16) {
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
    });
    MLOG("camera: %llu candidate matrices", static_cast<unsigned long long>(cands.size()));
    if (cands.empty()) return;

    // 2) Which of them change when the camera moves: compare again after a moment.
    Sleep(500);
    for (size_t i = 0; i < cands.size(); ++i) {
        float now[16];
        const bool ok = mem::read(cands[i].addr, now, 64);
        float diff = 0;
        for (int k = 0; ok && k < 16; ++k) diff = std::max(diff, std::fabs(now[k] - cands[i].m[k]));
        MLOG("camera: [%llu] %p %s w(eye)=%.2f changed=%.4f | %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.2f %.2f %.2f %.2f",
             static_cast<unsigned long long>(i), reinterpret_cast<void*>(cands[i].addr), cands[i].transposed ? "col" : "row",
             cands[i].w, ok ? diff : -1.f, cands[i].m[0], cands[i].m[1], cands[i].m[2], cands[i].m[3], cands[i].m[4],
             cands[i].m[5], cands[i].m[6], cands[i].m[7], cands[i].m[8], cands[i].m[9], cands[i].m[10], cands[i].m[11],
             cands[i].m[12], cands[i].m[13], cands[i].m[14], cands[i].m[15]);
    }

    // 3) Who points at the object around each candidate: globals directly, or through one heap object.
    constexpr uintptr_t kObjBefore = 0x2000; // a pointer up to this far before the matrix counts as "its object"
    auto candFor = [&](uintptr_t p) -> long {
        for (size_t i = 0; i < cands.size(); ++i)
            if (p <= cands[i].addr && cands[i].addr - p <= kObjBefore) return static_cast<long>(i);
        return -1;
    };
    int direct = 0;
    forEachImageGlobal([&](uintptr_t at, uintptr_t v) {
        const long c = v ? candFor(v) : -1;
        if (c < 0 || direct >= 40) return;
        ++direct;
        MLOG("camera: global 0x%llx -> object, matrix [%ld] at +0x%llx", static_cast<unsigned long long>(toVa(at)), c,
             static_cast<unsigned long long>(cands[c].addr - v));
    });
    std::vector<Holder> holders;
    forEachHeapChunk(buf, [&](uintptr_t base, const uint8_t* p, size_t n) {
        const auto* q = reinterpret_cast<const uintptr_t*>(p);
        for (size_t i = 0; i < n / 8 && holders.size() < 4096; ++i) {
            const long c = q[i] ? candFor(q[i]) : -1;
            if (c >= 0) holders.push_back({base + i * 8, q[i], static_cast<size_t>(c)});
        }
    });
    std::sort(holders.begin(), holders.end(), [](const Holder& a, const Holder& b) { return a.at < b.at; });
    int chains = 0;
    constexpr uintptr_t kHolderBefore = 0x800;
    forEachImageGlobal([&](uintptr_t at, uintptr_t v) {
        if (!v || chains >= 60) return;
        // holders whose address is in [v, v + kHolderBefore]
        auto it = std::lower_bound(holders.begin(), holders.end(), v, [](const Holder& h, uintptr_t x) { return h.at < x; });
        for (; it != holders.end() && it->at - v <= kHolderBefore && chains < 60; ++it, ++chains)
            MLOG("camera: global 0x%llx -> +0x%llx -> object, matrix [%llu] at +0x%llx",
                 static_cast<unsigned long long>(toVa(at)), static_cast<unsigned long long>(it->at - v),
                 static_cast<unsigned long long>(it->cand), static_cast<unsigned long long>(cands[it->cand].addr - it->target));
    });
    MLOG("camera: done in %.1f s (%d direct, %d through one object). Matrices at +0x%llx of their object are the "
         "render view's.", (GetTickCount() - start) / 1000.0, direct, chains, static_cast<unsigned long long>(kViewProjOffset));
}

} // namespace mecmp::game::camera_finder
