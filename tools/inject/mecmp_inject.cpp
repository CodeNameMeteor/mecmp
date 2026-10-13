// mecmp_inject: loads mecmp.dll into a running MirrorsEdgeCatalyst.exe with the standard
// LoadLibraryW method, and reports every step so failures are visible.
//
// Usage: mecmp_inject.exe [path\to\mecmp.dll]
//   Without an argument it uses mecmp.dll from its own folder.
// Run it once the game is at the main menu (or earlier). If OpenProcess fails with
// error 5, run it as administrator (the game is running elevated).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>

namespace {

const wchar_t* kGameExe = L"MirrorsEdgeCatalyst.exe";

DWORD findProcess(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

// Returns true if a module whose full path equals `path` (case-insensitive) is loaded in pid.
bool moduleLoaded(DWORD pid, const wchar_t* path, bool printList) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        wprintf(L"  (could not list modules: error %lu)\n", GetLastError());
        return false;
    }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsicmp(me.szExePath, path) == 0) found = true;
        if (printList && (wcsstr(me.szModule, L"mecmp") || _wcsicmp(me.szModule, L"version.dll") == 0))
            wprintf(L"  module: %ls  @%p\n", me.szExePath, static_cast<void*>(me.modBaseAddr));
    }
    CloseHandle(snap);
    return found;
}

int fail(const wchar_t* what) {
    DWORD e = GetLastError();
    wchar_t msg[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, msg, 512, nullptr);
    wprintf(L"FAILED: %ls (error %lu: %ls)\n", what, e, msg);
    if (e == ERROR_ACCESS_DENIED) wprintf(L"Hint: run mecmp_inject.exe as administrator.\n");
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t dll[MAX_PATH];
    if (argc > 1) {
        const DWORD n = GetFullPathNameW(argv[1], MAX_PATH, dll, nullptr);
        if (n == 0 || n >= MAX_PATH) return fail(L"GetFullPathName");
    } else {
        DWORD n = GetModuleFileNameW(nullptr, dll, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return fail(L"GetModuleFileName");
        wchar_t* slash = wcsrchr(dll, L'\\');
        if (!slash) return fail(L"bad path");
        slash[1] = 0;
        wcscat_s(dll, L"mecmp.dll");
    }
    wprintf(L"DLL: %ls\n", dll);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) return fail(L"mecmp.dll not found");

    DWORD pid = findProcess(kGameExe);
    if (!pid) {
        wprintf(L"Waiting for %ls to start...\n", kGameExe);
        while (!(pid = findProcess(kGameExe))) Sleep(500);
        Sleep(3000); // let the process initialise
    }
    wprintf(L"Game PID: %lu\n", pid);

    if (moduleLoaded(pid, dll, false)) {
        wprintf(L"mecmp.dll is already loaded in the game. Nothing to do.\n");
        return 0;
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) return fail(L"OpenProcess");

    const SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return fail(L"VirtualAllocEx");
    if (!WriteProcessMemory(proc, remote, dll, bytes, nullptr)) return fail(L"WriteProcessMemory");

    // kernel32 is mapped at the same address in every process of a boot session.
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!thread) return fail(L"CreateRemoteThread");
    wprintf(L"LoadLibraryW thread started, waiting...\n");
    if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
        wprintf(L"FAILED: LoadLibraryW did not return within 30s.\n");
        return 1;
    }
    // The thread's exit code is only the low 32 bits of the HMODULE, so ask the game whether the module is there.
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (!moduleLoaded(pid, dll, true)) {
        wprintf(L"FAILED: LoadLibraryW returned 0 inside the game. The DLL could not be loaded\n"
                L"(missing dependency, blocked file, or wrong architecture).\n");
        return 1;
    }
    wprintf(L"OK: mecmp.dll is loaded. The game shows the MECMP menu with INSERT; the log is in\n"
            L"the folder of mecmp.dll, else the game folder, else %%LOCALAPPDATA%%\\MECMP.\n");
    return 0;
}
