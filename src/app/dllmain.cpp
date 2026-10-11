// mecmp.dll entry point. Loaded into the running game by mecmp_inject.exe (or the optional
// version.dll proxy). All work happens on the mod's own thread and inside the game's tick.

#include <cwchar>

#include "app/mod.h"

namespace {
HMODULE g_self = nullptr;

DWORD WINAPI modThread(LPVOID) {
    // One instance per process: a second copy (e.g. proxy-loaded AND injected) stays idle.
    wchar_t name[64];
    swprintf(name, 64, L"Local\\MECMP_core_%lu", GetCurrentProcessId());
    HANDLE instance = CreateMutexW(nullptr, TRUE, name);
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        OutputDebugStringW(L"[MECMP] already active in this process; this copy stays idle\n");
        return 0;
    }
    Beep(750, 100); // audible "DLL loaded"
    mecmp::app::run(g_self);
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Pin the DLL: the hooks point into it, so it must never be unloaded while the game runs
        // (a stray FreeLibrary would otherwise leave the game calling freed code).
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        if (HANDLE t = CreateThread(nullptr, 0, modThread, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        // Process exit (the DLL is pinned, so there is no other detach): flush and close the log.
        mecmp::app::shutdown();
    }
    return TRUE;
}
