// mecmp_inject: loads mecmp.dll into the running game with the standard LoadLibraryW method, and
// reports every step so failures are visible.
//
// Usage: mecmp_inject.exe [path\to\mecmp.dll]
//   Without an argument it uses mecmp.dll from its own folder.
//
// It can be started before the game. Starting the game runs MirrorsEdgeCatalyst.exe twice: first the
// activation (EA app / OOA) process, then, once that is done, the game itself under the same name. Only
// the game loads the engine's modules, so a MirrorsEdgeCatalyst.exe counts as the game once
// Engine.BuildInfo_Win64_retail.dll is loaded in it and it has a visible window; the activation process
// is waited out. If the game runs elevated, run this as administrator too.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

const wchar_t* kGameExe = L"MirrorsEdgeCatalyst.exe";
const wchar_t* kEngineModule = L"Engine.BuildInfo_Win64_retail.dll"; // only the game itself loads it
constexpr DWORD kSettleMs = 3000; // after the game shows up: let it finish starting

std::vector<DWORD> gameProcesses() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, kGameExe) == 0) pids.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return pids;
}

enum class ModuleList { Ok, Denied, Gone };

// Calls `fn` for each module of pid; tells whether the list could be read.
template <class Fn> ModuleList forEachModule(DWORD pid, Fn fn) {
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 5 && snap == INVALID_HANDLE_VALUE; ++tries) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break; // ERROR_BAD_LENGTH: retry
    }
    if (snap == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_ACCESS_DENIED ? ModuleList::Denied : ModuleList::Gone;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) fn(me);
    CloseHandle(snap);
    return ModuleList::Ok;
}

bool hasVisibleWindow(DWORD pid) {
    struct Ctx { DWORD pid; bool found; } ctx{pid, false};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD owner = 0;
            GetWindowThreadProcessId(h, &owner);
            if (owner == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

struct Candidate {
    DWORD pid = 0;
    bool denied = false; // a MirrorsEdgeCatalyst.exe we may not look into (it runs elevated)
};

// The game's process: the MirrorsEdgeCatalyst.exe with the engine loaded and a window up.
Candidate findGame() {
    Candidate c;
    for (DWORD pid : gameProcesses()) {
        bool engine = false;
        const ModuleList r = forEachModule(pid, [&](const MODULEENTRY32W& me) {
            if (_wcsicmp(me.szModule, kEngineModule) == 0) engine = true;
        });
        if (r == ModuleList::Denied) c.denied = true;
        if (r == ModuleList::Ok && engine && hasVisibleWindow(pid)) c.pid = pid;
    }
    return c;
}

// Where mecmp.dll is loaded in pid from (empty: not loaded).
std::wstring loadedMecmp(DWORD pid) {
    std::wstring path;
    forEachModule(pid, [&](const MODULEENTRY32W& me) {
        if (_wcsicmp(me.szModule, L"mecmp.dll") == 0) path = me.szExePath;
    });
    return path;
}

int fail(const wchar_t* what) {
    DWORD e = GetLastError();
    wchar_t msg[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, msg, 512, nullptr);
    wprintf(L"FAILED: %ls (error %lu: %ls)\n", what, e, msg);
    if (e == ERROR_ACCESS_DENIED) wprintf(L"Hint: run mecmp_inject.exe as administrator.\n");
    return 1;
}

// Started by double-clicking, the console window closes with us. After a success it stays up for 10 s (a
// key closes it at once); after a failure it waits for a key, so the error can be read.
constexpr DWORD kCloseAfterMs = 10000;

int finish(int code) {
    DWORD list[2];
    if (GetConsoleProcessList(list, 2) != 1) return code;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(in);
    const bool timed = code == 0;
    if (!timed) wprintf(L"\nPress any key to close this window.\n");
    const ULONGLONG end = GetTickCount64() + kCloseAfterMs;
    int shown = -1;
    for (;;) {
        DWORD wait = INFINITE;
        if (timed) {
            const ULONGLONG now = GetTickCount64();
            if (now >= end) break;
            const int left = static_cast<int>((end - now + 999) / 1000);
            if (left != shown) {
                shown = left;
                wprintf(L"\rThis window closes in %2d s (any key closes it now). ", left);
            }
            wait = static_cast<DWORD>((end - now) % 1000 + 1);
        }
        if (WaitForSingleObject(in, wait) != WAIT_OBJECT_0) continue;
        INPUT_RECORD rec;
        DWORD n = 0;
        if (!ReadConsoleInputW(in, &rec, 1, &n) || !n) break;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) break;
    }
    if (timed) wprintf(L"\n");
    return code;
}

int inject(DWORD pid, const wchar_t* dll) {
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
    wprintf(L"Loading mecmp.dll into the game...\n");
    if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
        wprintf(L"FAILED: LoadLibraryW did not return within 30 s.\n");
        return 1;
    }
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    // The thread's exit code is only the low 32 bits of the HMODULE, so ask the game whether the module is there.
    if (loadedMecmp(pid).empty()) {
        wprintf(L"FAILED: the game could not load mecmp.dll (missing dependency, blocked file, or wrong\n"
                L"architecture). Windows may block downloaded files: right-click mecmp.dll, Properties, Unblock.\n");
        return 1;
    }
    wprintf(L"OK: mecmp.dll is loaded. Press INSERT in game for the MECMP menu. The log, mecmp.log, is in\n"
            L"the folder of mecmp.dll (else the game folder, else %%LOCALAPPDATA%%\\MECMP).\n");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t dll[MAX_PATH];
    if (argc > 1) {
        const DWORD n = GetFullPathNameW(argv[1], MAX_PATH, dll, nullptr);
        if (n == 0 || n >= MAX_PATH) return finish(fail(L"GetFullPathName"));
    } else {
        DWORD n = GetModuleFileNameW(nullptr, dll, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return finish(fail(L"GetModuleFileName"));
        wchar_t* slash = wcsrchr(dll, L'\\');
        if (!slash) return finish(fail(L"bad path"));
        slash[1] = 0;
        wcscat_s(dll, L"mecmp.dll");
    }
    wprintf(L"MECMP injector\nDLL: %ls\n", dll);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"FAILED: mecmp.dll not found. Keep mecmp_inject.exe next to mecmp.dll.\n");
        return finish(1);
    }

    Candidate game = findGame();
    if (!game.pid) {
        bool saidActivation = false, saidDenied = false;
        wprintf(L"Waiting for Mirror's Edge Catalyst (start it now; Ctrl+C cancels)...\n");
        for (;;) {
            game = findGame();
            if (game.pid) break;
            if (game.denied && !saidDenied) {
                saidDenied = true;
                wprintf(L"The game runs as administrator: close this and run mecmp_inject.exe as administrator.\n");
            }
            if (!saidActivation && !gameProcesses().empty()) {
                saidActivation = true;
                wprintf(L"The activation window is up; waiting for the game itself...\n");
            }
            Sleep(500);
        }
        wprintf(L"Game found; giving it a moment to finish starting...\n");
        Sleep(kSettleMs);
        const DWORD pid = game.pid;
        game = findGame();
        if (game.pid != pid) {
            wprintf(L"FAILED: the game closed while starting.\n");
            return finish(1);
        }
    }
    wprintf(L"Game PID: %lu\n", game.pid);

    const std::wstring already = loadedMecmp(game.pid);
    if (!already.empty()) {
        if (_wcsicmp(already.c_str(), dll) == 0) wprintf(L"mecmp.dll is already loaded in the game. Nothing to do.\n");
        else wprintf(L"The game already has a mecmp.dll, from %ls.\nRestart the game to use this one.\n", already.c_str());
        return finish(0);
    }
    return finish(inject(game.pid, dll));
}
