#include "core/config.h"
#include <cstdio>
#include <filesystem>
#include <windows.h>

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const auto dir = std::filesystem::path(temp) / (L"mecmp-main-server-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(dir)) return 1;
    mecmp::config::load(dir.wstring());
    bool ok = mecmp::config::get()->mainServer == "34.27.13.116:47800";
    for (const char* address : {"127.0.0.1:47801", ""}) {
        mecmp::config::change([address](auto& c) { c.mainServer = address; });
        mecmp::config::applyPending();
        Sleep(450);
        mecmp::config::applyPending();
        mecmp::config::load(dir.wstring());
        ok = ok && mecmp::config::get()->mainServer == address;
    }
    std::filesystem::remove(dir / L"mecmp.ini");
    std::filesystem::remove(dir);
    std::printf("main server config: %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}
