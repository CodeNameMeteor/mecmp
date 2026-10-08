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

namespace mecmp::mem {

inline uintptr_t moduleBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// VA as seen in Ghidra (PC image base 0x140000000) -> address in the running process.
inline uintptr_t rva(uintptr_t va, uintptr_t imageBase = 0x140000000) { return moduleBase() + (va - imageBase); }

inline bool read(uintptr_t addr, void* out, size_t size) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
}

template <class T> inline bool read(uintptr_t addr, T& out) { return read(addr, &out, sizeof(T)); }

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
