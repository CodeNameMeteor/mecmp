#pragma once
// Fault-tolerant reads of game memory. Uses ReadProcessMemory on our own process,
// which returns an error instead of crashing when a page is not readable. It is
// slower than a plain load, so use it for discovery and validation, not per frame.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/prof.h"

namespace mecmp::mem {

inline uintptr_t moduleBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// VA as seen in Ghidra (PC image base 0x140000000) -> address in the running process.
inline uintptr_t rva(uintptr_t va, uintptr_t imageBase = 0x140000000) { return moduleBase() + (va - imageBase); }

inline bool read(uintptr_t addr, void* out, size_t size) {
    if (addr < 0x10000) return false;
    ++prof::t_reads;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
}

template <class T> inline bool read(uintptr_t addr, T& out) { return read(addr, &out, sizeof(T)); }

// Fault-tolerant write into our own process. NtWriteVirtualMemory is the kernel call under
// WriteProcessMemory without the extras: WriteProcessMemory also queries and changes the page
// protection and flushes the instruction cache on every call (it is made for patching code), which
// costs ~0.3 ms for a ghost's pose. Falls back to WriteProcessMemory (e.g. for a read-only page).
inline bool write(uintptr_t addr, const void* src, size_t size) {
    if (addr < 0x10000) return false;
    ++prof::t_writes;
    using NtWriteFn = LONG(NTAPI*)(HANDLE, PVOID, LPCVOID, SIZE_T, SIZE_T*);
    static const NtWriteFn ntWrite =
        reinterpret_cast<NtWriteFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWriteVirtualMemory")));
    SIZE_T done = 0;
    if (ntWrite && ntWrite(GetCurrentProcess(), reinterpret_cast<PVOID>(addr), src, size, &done) >= 0 && done == size)
        return true;
    done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), src, size, &done) && done == size;
}

// The game image's data sections (vtables, type info, globals): committed, readable and not code,
// found once with VirtualQuery. They stay mapped for the life of the process, so a read that lies
// entirely inside one is a plain copy instead of a kernel call.
struct Range { uintptr_t begin, end; };
struct DataRanges {
    Range r[32];
    int n = 0;
};
inline const DataRanges& imageDataRanges() {
    static const DataRanges ranges = [] {
        DataRanges d;
        const uintptr_t base = moduleBase();
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
        const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
        for (uintptr_t a = base; a < end && d.n < 32;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) || !mbi.RegionSize) break;
            const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
            const DWORD p = mbi.Protect;
            const bool data = mbi.State == MEM_COMMIT && !(p & (PAGE_GUARD | PAGE_NOACCESS)) &&
                              (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)); // not executable
            if (data) {
                if (d.n && d.r[d.n - 1].end == rb) d.r[d.n - 1].end = re < end ? re : end;
                else d.r[d.n++] = {rb, re < end ? re : end};
            }
            a = re;
        }
        return d;
    }();
    return ranges;
}

inline bool inImageData(uintptr_t addr, size_t size) {
    const DataRanges& d = imageDataRanges();
    for (int i = 0; i < d.n; ++i)
        if (addr >= d.r[i].begin && addr + size <= d.r[i].end && addr + size >= addr) return true;
    return false;
}

// As read(), but a plain copy when the bytes lie in the game image's data (see above).
inline bool readFast(uintptr_t addr, void* out, size_t size) {
    if (inImageData(addr, size)) {
        memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    }
    return read(addr, out, size);
}
template <class T> inline bool readFast(uintptr_t addr, T& out) { return readFast(addr, &out, sizeof(T)); }

template <class T> inline T readOr(uintptr_t addr, T fallback = T{}) {
    T v;
    return read(addr, v) ? v : fallback;
}

// Reads a NUL-terminated string of printable ASCII (returns false if none / not printable).
inline bool readCString(uintptr_t addr, std::string& out, size_t maxLen = 256) {
    out.clear();
    char buf[64];
    while (out.size() < maxLen) {
        if (!read(addr + out.size(), buf, sizeof(buf))) {
            // Retry byte by byte near a page boundary.
            char c;
            if (!read(addr + out.size(), c)) return false;
            if (c == 0) return !out.empty();
            if (c < 0x20 || c > 0x7e) return false;
            out.push_back(c);
            continue;
        }
        for (char c : buf) {
            if (c == 0) return !out.empty();
            if (c < 0x20 || c > 0x7e) return false;
            out.push_back(c);
            if (out.size() >= maxLen) return false;
        }
    }
    return false;
}

// As readCString, but for text players wrote (a time trial's name): UTF-8 is kept. Control characters and
// bytes that are not valid UTF-8 still make it false, so a wrong pointer is not taken for a name.
inline bool readUtf8String(uintptr_t addr, std::string& out, size_t maxLen = 256) {
    out.clear();
    std::string raw;
    char c = 0;
    while (raw.size() <= maxLen) {
        if (!read(addr + raw.size(), c)) return false;
        if (c == 0) break;
        raw.push_back(c);
    }
    if (raw.empty() || raw.size() > maxLen) return false;
    for (size_t i = 0; i < raw.size();) {
        const unsigned char b = static_cast<unsigned char>(raw[i]);
        if (b < 0x80) {
            if (b < 0x20 || b == 0x7f) return false;
            ++i;
            continue;
        }
        const size_t n = (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (!n || (n == 2 && b < 0xC2) || b > 0xF4 || i + n > raw.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(raw[i + k]) >> 6) != 2) return false;
        i += n;
    }
    out = raw;
    return true;
}

// Writes one pointer-sized value into read-only data (a vtable slot). Returns the old value.
inline bool patchPointer(uintptr_t addr, uintptr_t value, uintptr_t* old) {
    DWORD prot = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), PAGE_READWRITE, &prot)) return false;
    if (old) *old = *reinterpret_cast<volatile uintptr_t*>(addr);
    // Aligned 8-byte store: atomic on x64, so a concurrent virtual call sees old or new.
    *reinterpret_cast<volatile uintptr_t*>(addr) = value;
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

} // namespace mecmp::mem
