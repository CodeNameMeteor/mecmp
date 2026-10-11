#include "app/diagnostics.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/mem.h"

namespace mecmp::app::diagnostics {
namespace {
HMODULE g_self = nullptr;

// "<module>+0x<offset>" for the game and the mod, "?" otherwise. Uses only addresses captured at
// install time: no loader lock, no heap (the faulting thread may hold either).
uintptr_t g_gameBase = 0, g_selfBase = 0, g_selfSize = 0;
constexpr uintptr_t kGameImageSize = 0x727b000;

void describeAddress(uintptr_t a, char* out, size_t n) {
    if (a >= g_gameBase && a < g_gameBase + kGameImageSize)
        snprintf(out, n, "game+0x%llx", static_cast<unsigned long long>(a - g_gameBase));
    else if (a >= g_selfBase && a < g_selfBase + g_selfSize)
        snprintf(out, n, "mecmp+0x%llx", static_cast<unsigned long long>(a - g_selfBase));
    else
        snprintf(out, n, "?");
}

// Vectored handler: sees every first-chance exception in the process, on the faulting thread,
// before the game's own handlers. Logs the first few crash-type ones and lets them continue.
// Nothing here may take a lock it could wait on (B-15): stack buffers only, writeUrgent.
LONG CALLBACK onException(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> logged{0};
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    if (logged.fetch_add(1) >= 5) return EXCEPTION_CONTINUE_SEARCH;
    // Re-entry on the same thread (a fault inside this handler) must not recurse.
    static thread_local bool inside = false;
    if (inside) return EXCEPTION_CONTINUE_SEARCH;
    inside = true;
    const uintptr_t pc = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    const uintptr_t target = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    char where[64], line[256];
    describeAddress(pc, where, sizeof(where));
    snprintf(line, sizeof(line), "EXCEPTION 0x%08lx at %p (%s), address %p, thread %lu (the game may handle this itself)",
             static_cast<unsigned long>(code), reinterpret_cast<void*>(pc), where, reinterpret_cast<void*>(target),
             GetCurrentThreadId());
    log::writeUrgent(line);
    // A stack overflow leaves no room to walk the stack.
    if (code != EXCEPTION_STACK_OVERFLOW) {
        const auto* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
        for (int i = 0, shown = 0; i < 256 && shown < 10; ++i) {
            uintptr_t v = 0;
            if (!mem::read(reinterpret_cast<uintptr_t>(sp + i), v)) break;
            if ((v > g_gameBase + 0x1000 && v < g_gameBase + kGameImageSize) || (v > g_selfBase && v < g_selfBase + g_selfSize)) {
                describeAddress(v, where, sizeof(where));
                snprintf(line, sizeof(line), "  stack[%03d] %s", i, where);
                log::writeUrgent(line);
                ++shown;
            }
        }
    }
    inside = false;
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void install(HMODULE self) {
    g_self = self;
    g_gameBase = mem::moduleBase();
    g_selfBase = reinterpret_cast<uintptr_t>(self);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
    g_selfSize = nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, onException);
}

} // namespace mecmp::app::diagnostics
