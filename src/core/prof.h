#pragma once
// A small always-on profiler: what the mod's hooks cost the game, per section, between two F9 presses.
// Each section counts calls, time (avg / max) and how many fault-tolerant memory reads and writes
// (ReadProcessMemory / WriteProcessMemory, each a kernel call) it made per call.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mecmp::prof {

enum Section : int {
    GameTick,    // the server tick hook, everything below on the game thread
    Capture,     //   reading the local player's pose
    Ghosts,      //   driving the ghosts
    Teleport,    //   teleport / summon
    Ping,        //   markers
    Models,      //   look catalogue
    RenderPose,  // client replay update: the ghosts' poses for this frame
    RenderState, //   the blended network state (net::remoteState)
    RenderRead,  //   reading the ghost's client component
    RenderWrite, //   writing the pose
    UIPause,     // the UI system hook (unused since menus pause again)
    Overlay,     // the overlay (ImGui) on Present
    kSections
};

inline const char* name(int s) {
    static const char* const k[kSections] = {"tick", "capture", "ghosts", "teleport", "ping",
                                             "models", "render pose", "  state", "  read", "  write", "ui", "overlay"};
    return s >= 0 && s < kSections ? k[s] : "?";
}

// Kernel calls made by this thread (mem::read, poke). Plain thread-locals: cheap to bump.
inline thread_local uint32_t t_reads = 0;
inline thread_local uint32_t t_writes = 0;

struct Stat {
    std::atomic<uint64_t> calls{0}, ticks{0}, maxTicks{0}, reads{0}, writes{0};
};
inline Stat g_stats[kSections];
inline std::atomic<DWORD> g_since{0};

inline uint64_t now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

inline double ticksToUs(uint64_t t) {
    static const double perUs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1e6;
    }();
    return static_cast<double>(t) / perUs;
}

class Scope {
public:
    explicit Scope(Section s) : s_(s), t0_(now()), r0_(t_reads), w0_(t_writes) {}
    ~Scope() {
        const uint64_t dt = now() - t0_;
        Stat& st = g_stats[s_];
        st.calls.fetch_add(1, std::memory_order_relaxed);
        st.ticks.fetch_add(dt, std::memory_order_relaxed);
        st.reads.fetch_add(t_reads - r0_, std::memory_order_relaxed);
        st.writes.fetch_add(t_writes - w0_, std::memory_order_relaxed);
        uint64_t m = st.maxTicks.load(std::memory_order_relaxed);
        while (dt > m && !st.maxTicks.compare_exchange_weak(m, dt, std::memory_order_relaxed)) {}
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Section s_;
    uint64_t t0_;
    uint32_t r0_, w0_;
};

// "tick 120 us avg / 5.2 ms max, 38 reads 3 writes per call, 60/s | ..." for the sections that ran,
// over the time since the last report; then starts a new window.
inline std::string report() {
    const DWORD nowMs = GetTickCount();
    DWORD since = g_since.exchange(nowMs);
    const double secs = since ? (nowMs - since) / 1000.0 : 0.0;
    std::string out;
    for (int i = 0; i < kSections; ++i) {
        Stat& st = g_stats[i];
        const uint64_t calls = st.calls.exchange(0), ticks = st.ticks.exchange(0), maxT = st.maxTicks.exchange(0);
        const uint64_t reads = st.reads.exchange(0), writes = st.writes.exchange(0);
        if (!calls) continue;
        char line[200];
        const double avg = ticksToUs(ticks) / static_cast<double>(calls), mx = ticksToUs(maxT);
        snprintf(line, sizeof(line), "%s%s %.0f us avg / %.0f us max, %.1f reads %.1f writes per call", out.empty() ? "" : " | ",
                 name(i), avg, mx, static_cast<double>(reads) / static_cast<double>(calls),
                 static_cast<double>(writes) / static_cast<double>(calls));
        out += line;
        if (secs > 0.5) {
            snprintf(line, sizeof(line), ", %.0f/s", static_cast<double>(calls) / secs);
            out += line;
        }
    }
    if (out.empty()) out = "nothing measured yet";
    if (secs > 0) {
        char head[48];
        snprintf(head, sizeof(head), "(%.0f s) ", secs);
        out = head + out;
    }
    return out;
}

} // namespace mecmp::prof
