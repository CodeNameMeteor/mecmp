#include "core/config.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
int main() {
    wchar_t temp[MAX_PATH];GetTempPathW(MAX_PATH,temp);
    const auto dir=std::filesystem::path(temp)/(L"mecmp-door-config-"+std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(dir)) return 1;
    mecmp::config::load(dir.wstring());
    bool ok=mecmp::config::get()->syncDoors&&mecmp::config::get()->doorKey.empty();
    mecmp::config::change([](auto& c){c.syncDoors=false;c.doorKey="J";});
    mecmp::config::applyPending();
    Sleep(450);mecmp::config::applyPending(); // Settings are deliberately saved after a quiet interval.
    mecmp::config::load(dir.wstring());
    ok=ok&&!mecmp::config::get()->syncDoors&&mecmp::config::get()->doorKey=="J";
    std::filesystem::remove(dir/L"mecmp.ini");std::filesystem::remove(dir);
    std::printf("door config test: %s\n",ok?"passed":"FAILED");return ok?0:1;
}
