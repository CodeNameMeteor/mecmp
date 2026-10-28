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
    bool ok = mecmp::config::get()->mainServer == mecmp::config::kDefaultMainServer;
    const auto ini = (dir / L"mecmp.ini").wstring();
    auto check = [&](bool condition, const char* message) {
        if (!condition) std::printf("FAILED: %s\n", message);
        ok = condition && ok;
    };
    auto savedAddress = [&] {
        wchar_t value[256];
        GetPrivateProfileStringW(L"net", L"main_server", L"<missing>", value, 256, ini.c_str());
        return std::wstring(value);
    };
    auto save = [] {
        mecmp::config::applyPending(true);
    };
    check(savedAddress() == L"<missing>", "fresh ini pins the default server");
    mecmp::config::change([](auto& c) { c.name = "Config test"; });
    save();
    check(savedAddress() == L"<missing>", "unrelated setting pins the default server");
    for (const char* address : {"127.0.0.1:47801", "", mecmp::config::kDefaultMainServer}) {
        mecmp::config::change([address](auto& c) { c.mainServer = address; });
        save();
        if (std::string(address) == mecmp::config::kDefaultMainServer)
            check(savedAddress() == L"<missing>", "returning to default leaves an override");
        else
            check(savedAddress() == (address[0] ? L"127.0.0.1:47801" : L""), "custom or empty override not saved");
        mecmp::config::load(dir.wstring());
        check(mecmp::config::get()->mainServer == address, "address changed after reload");
    }
    const std::wstring defaultAddress(mecmp::config::kDefaultMainServer,
                                      mecmp::config::kDefaultMainServer + std::char_traits<char>::length(mecmp::config::kDefaultMainServer));
    WritePrivateProfileStringW(L"net", L"main_server", defaultAddress.c_str(), ini.c_str());
    mecmp::config::load(dir.wstring());
    mecmp::config::change([](auto& c) { c.name = "Old ini"; });
    save();
    check(savedAddress() == L"<missing>", "existing default override not removed on save");
    std::filesystem::remove(dir / L"mecmp.ini");
    std::filesystem::remove(dir);
    std::printf("main server config: %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}
