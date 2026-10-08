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

std::string describeAddress(uintptr_t a) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &mod) && mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        char buf[MAX_PATH + 32];
        snprintf(buf, sizeof(buf), "%s+0x%llx", name ? name + 1 : path,
                 static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(mod)));
        return buf;
    }
    return "?";
}

LONG CALLBACK onException(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> logged{0};
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    if (logged.fetch_add(1) >= 5) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t pc = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    const uintptr_t target = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    MLOG("EXCEPTION 0x%08lx at %p (%s), address %p, thread %lu (the game may handle this itself)",
         static_cast<unsigned long>(code), reinterpret_cast<void*>(pc), describeAddress(pc).c_str(),
         reinterpret_cast<void*>(target), GetCurrentThreadId());
    const auto* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
    const uintptr_t base = mem::moduleBase(), self = reinterpret_cast<uintptr_t>(g_self);
    for (int i = 0, shown = 0; i < 256 && shown < 10; ++i) {
        uintptr_t v = 0;
        if (!mem::read(reinterpret_cast<uintptr_t>(sp + i), v)) break;
        if ((v > base + 0x1000 && v < base + 0x727b000) || (v > self && v < self + 0x100000)) {
            MLOG("  stack[%03d] %s", i, describeAddress(v).c_str());
            ++shown;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void install(HMODULE self) {
    g_self = self;
    AddVectoredExceptionHandler(1, onException);
}

} // namespace mecmp::app::diagnostics
