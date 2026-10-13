#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "ui/overlay.h"

#include <atomic>
#include <cctype>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "core/config.h"
#include "core/log.h"
#include "core/mem.h"
#include "game/ghosts.h"
#include "game/hooks.h"
#include "game/teleport.h"
#include "game/mumble_link.h"
#include "imgui.h"
#include "ui/chat.h"
#include "ui/gamepad.h"
#include "ui/markers.h"
#include "ui/menu.h"
#include "ui/nametags.h"
#include "ui/notices.h"
#include "ui/radial.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace mecmp::ui {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn g_origPresent = nullptr;
ResizeFn g_origResize = nullptr;

IDXGISwapChain* g_swapChain = nullptr; // the game's (the first one presented with a real window)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_window = nullptr;
WNDPROC g_origWndProc = nullptr;
std::atomic<void (*)()> g_onDestroyed{nullptr};
bool g_ready = false;
std::atomic<bool> g_open{false};
std::atomic<int> g_toggleKey{VK_INSERT};
std::atomic<bool> g_keyCapture{false};
std::atomic<bool> g_justOpened{false}; // focus the menu window on the next frame (gamepad navigation)
std::atomic<int> g_chatKey{'T'};
std::atomic<int> g_pingKey{0}; // 0 = none
std::atomic<int> g_pingClearKey{0}; // a key, or VK_MBUTTON / VK_XBUTTON1 / VK_XBUTTON2; 0 = none
bool g_eatChar = false; // the character typed by the key that opened the chat (window thread)
int g_openingChatKey = 0;

const MenuKey kMenuKeys[] = {
    {"INSERT", VK_INSERT, ImGuiKey_Insert}, {"HOME", VK_HOME, ImGuiKey_Home}, {"END", VK_END, ImGuiKey_End},
    {"DELETE", VK_DELETE, ImGuiKey_Delete}, {"PAGEUP", VK_PRIOR, ImGuiKey_PageUp}, {"PAGEDOWN", VK_NEXT, ImGuiKey_PageDown},
    {"PAUSE", VK_PAUSE, ImGuiKey_Pause}, {"F1", VK_F1, ImGuiKey_F1}, {"F2", VK_F2, ImGuiKey_F2}, {"F3", VK_F3, ImGuiKey_F3},
    {"F4", VK_F4, ImGuiKey_F4}, {"F5", VK_F5, ImGuiKey_F5}, {"F6", VK_F6, ImGuiKey_F6}, {"F7", VK_F7, ImGuiKey_F7},
    {"F8", VK_F8, ImGuiKey_F8}, {"F9", VK_F9, ImGuiKey_F9}, {"F10", VK_F10, ImGuiKey_F10}, {"F11", VK_F11, ImGuiKey_F11},
    {"F12", VK_F12, ImGuiKey_F12},
};

void createRtv() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

bool isInputMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_INPUT;
}

// Virtual cursor. In gameplay the game hides the cursor, keeps re-centring it and reads the mouse
// as raw input, so the real cursor is useless for the menu. While the menu is open we move our own
// cursor with the raw mouse movement (WM_INPUT) and feed it to ImGui; the legacy mouse messages
// are only used if no raw input arrives (e.g. in the game's own menus).
std::atomic<float> g_cursorX{0}, g_cursorY{0};
std::atomic<bool> g_rawMouse{false};

void clampCursor() {
    RECT rc{};
    GetClientRect(g_window, &rc);
    float x = g_cursorX, y = g_cursorY;
    x = x < 0 ? 0 : (x > rc.right - 1 ? rc.right - 1 : x);
    y = y < 0 ? 0 : (y > rc.bottom - 1 ? rc.bottom - 1 : y);
    g_cursorX = x;
    g_cursorY = y;
}

// Raw mouse events are collected here (window thread) and handed to ImGui at the start of the next
// frame (render thread): ImGui itself is not thread-safe.
std::atomic<uint32_t> g_btnDown{0}, g_btnUp{0};
std::atomic<int> g_wheel{0};

void handleRawMouse(LPARAM l) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri.data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
        g_cursorX = g_cursorX + static_cast<float>(m.lLastX);
        g_cursorY = g_cursorY + static_cast<float>(m.lLastY);
        clampCursor();
    }
    const USHORT b = m.usButtonFlags;
    uint32_t down = 0, up = 0;
    if (b & RI_MOUSE_LEFT_BUTTON_DOWN) down |= 1;
    if (b & RI_MOUSE_LEFT_BUTTON_UP) up |= 1;
    if (b & RI_MOUSE_RIGHT_BUTTON_DOWN) down |= 2;
    if (b & RI_MOUSE_RIGHT_BUTTON_UP) up |= 2;
    if (b & RI_MOUSE_MIDDLE_BUTTON_DOWN) down |= 4;
    if (b & RI_MOUSE_MIDDLE_BUTTON_UP) up |= 4;
    if (b & RI_MOUSE_BUTTON_4_DOWN) down |= 8;
    if (b & RI_MOUSE_BUTTON_4_UP) up |= 8;
    if (b & RI_MOUSE_BUTTON_5_DOWN) down |= 16;
    if (b & RI_MOUSE_BUTTON_5_UP) up |= 16;
    if (down) g_btnDown |= down;
    if (up) g_btnUp |= up;
    if (b & RI_MOUSE_WHEEL) g_wheel += static_cast<SHORT>(m.usButtonData);
    g_rawMouse = true;
}

// Render thread, before NewFrame: apply what the window thread collected.
void applyRawMouse() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(g_cursorX, g_cursorY);
    const uint32_t down = g_btnDown.exchange(0), up = g_btnUp.exchange(0);
    for (int i = 0; i < 5; ++i) {
        // A click that went down and up between two frames: press this frame, release the next.
        if (down & (1u << i)) io.AddMouseButtonEvent(i, true);
        if (up & (1u << i)) {
            if (down & (1u << i)) g_btnUp |= (1u << i);
            else io.AddMouseButtonEvent(i, false);
        }
    }
    if (const int w = g_wheel.exchange(0)) io.AddMouseWheelEvent(0.f, static_cast<float>(w) / WHEEL_DELTA);
}

// A raw keyboard event: 1 = key press, 2 = key release, 0 = not the keyboard.
int rawKeyEvent(LPARAM l) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return 0;
    if (ri.header.dwType != RIM_TYPEKEYBOARD) return 0;
    return (ri.data.keyboard.Flags & RI_KEY_BREAK) ? 2 : 1;
}
bool isRawKeyPress(LPARAM l) { return rawKeyEvent(l) == 1; }

bool isMouseMessage(UINT msg) { return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST; }

// Raw input while the marker wheel is open: mouse movement steers the wheel, a left click picks and
// the mouse wheel switches wheel. True for a mouse event (which then stays away from the game);
// keyboard input is not touched.
bool g_radialRaw = false; // raw mouse input arrives, so the legacy mouse messages are duplicates (window thread)
bool radialRawMouse(LPARAM l) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return false;
    if (ri.header.dwType != RIM_TYPEMOUSE) return false;
    const RAWMOUSE& m = ri.data.mouse;
    const USHORT b = m.usButtonFlags;
    g_radialRaw = true;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY))
        radial::moved(static_cast<float>(m.lLastX), static_cast<float>(m.lLastY));
    if (b & RI_MOUSE_LEFT_BUTTON_DOWN) radial::click();
    if (b & RI_MOUSE_WHEEL) radial::switchWheel(static_cast<SHORT>(m.usButtonData) > 0 ? -1 : 1);
    return true;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    const bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (keyUp && g_ready) ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
    if (msg == WM_KILLFOCUS) { g_openingChatKey = 0; g_eatChar = false; }
    const int openingChar = g_openingChatKey >= 'A' && g_openingChatKey <= 'Z' ? g_openingChatKey + ('a' - 'A') : g_openingChatKey;
    if (g_openingChatKey && ((msg == WM_KEYDOWN && static_cast<int>(w) == g_openingChatKey) ||
        (msg == WM_CHAR && (static_cast<int>(w) == g_openingChatKey || static_cast<int>(w) == openingChar)))) return 0;
    if (keyUp && static_cast<int>(w) == g_openingChatKey) { g_openingChatKey = 0; g_eatChar = false; }
    if (msg == WM_DESTROY && hwnd == g_window) {
        if (auto fn = g_onDestroyed.exchange(nullptr)) fn();
    }
    // The marker wheel (ui/radial.h). The keyboard still reaches the game, so keys are only noted here
    // and passed on; while the wheel is open it owns the mouse (the camera stays still).
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && w == FALSE)) radial::cancel();
    if (msg == WM_KEYUP && g_pingKey.load() != 0 && static_cast<int>(w) == g_pingKey.load()) radial::keyUp();
    if (radial::engaged() && msg == WM_KEYDOWN && !(l & (1 << 30))) {
        const int key = static_cast<int>(w);
        if (key >= '1' && key <= '9') radial::digit(key - '0');
        else if (key >= VK_NUMPAD1 && key <= VK_NUMPAD9) radial::digit(key - VK_NUMPAD0);
        else if (key == VK_LEFT) radial::switchWheel(-1);
        else if (key == VK_RIGHT) radial::switchWheel(1);
    }
    if (radial::open()) {
        if (msg == WM_INPUT && radialRawMouse(l)) return DefWindowProcW(hwnd, msg, w, l);
        if (isMouseMessage(msg)) {
            if (!g_radialRaw) { // no raw mouse input here: the legacy messages are all there is
                if (msg == WM_LBUTTONDOWN) radial::click();
                else if (msg == WM_MOUSEWHEEL) radial::switchWheel(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1);
            }
            return 0;
        }
    }
    // Only the menu key toggles the menu, never Escape: Escape reaches ImGui below (it cancels a key
    // capture and closes combo boxes) and nothing else. The menu has no close button by design.
    if (msg == WM_KEYDOWN && static_cast<int>(w) != VK_ESCAPE && static_cast<int>(w) == g_toggleKey.load() &&
        !(l & (1 << 30)) && !g_keyCapture.load()) {
        toggleMenu();
        return 0;
    }
    // Chat: its key opens the input line (not while the menu is open). While it is open the keyboard
    // and raw input go to it, not to the game; Escape cancels.
    // Not while one of the game's own menus is up (pause menu, map): Enter there picks a menu item.
    if (msg == WM_KEYDOWN && !g_open && !chat::isOpen() && g_ready && static_cast<int>(w) == g_chatKey.load() &&
        !(l & (1 << 30)) && game::hooks::gameMenuOpen())
        MLOG("chat: not opened (a game menu is up)");
    else if (msg == WM_KEYDOWN && !g_open && !chat::isOpen() && g_ready && static_cast<int>(w) == g_chatKey.load() &&
             !(l & (1 << 30))) {
        radial::cancel(); // the chat takes the keyboard
        chat::open();
        if (!chat::isOpen()) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
        g_openingChatKey = static_cast<int>(w);
        g_eatChar = true;
        return 0;
    }
    // Marker key: only noted here; the game still gets the key. A short press places a marker when
    // the key is released, holding it opens the marker wheel. Not over one of the game's own menus.
    if (msg == WM_KEYDOWN && !g_open && !chat::isOpen() && g_ready && g_pingKey.load() != 0 &&
        static_cast<int>(w) == g_pingKey.load() && !(l & (1 << 30)) && !game::hooks::gameMenuOpen())
        radial::keyDown(g_pingKey.load());
    if (chat::isOpen() && g_ready && !g_open) {
        if (msg == WM_KEYDOWN && w == VK_ESCAPE) { chat::close(); return 0; }
        if (msg == WM_CHAR && g_eatChar) { g_eatChar = false; return 0; }
        // Key presses go to the chat line only. Key releases and the mouse still reach the game, so a
        // key held when the chat opened is released there (Faith does not keep running).
        if (msg == WM_INPUT && isRawKeyPress(l)) return DefWindowProcW(hwnd, msg, w, l);
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR || msg == WM_SYSCHAR) {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
            return 0;
        }
    }
    // A pad plugged in later: let the backend look for it again (it only sets a flag).
    if (msg == WM_DEVICECHANGE && g_ready) ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
    if (g_open && g_ready) {
        // Key releases still reach the game (ImGui got them above), so a key held when the menu
        // opened is released there and Faith does not keep running.
        if (keyUp) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
        if (msg == WM_INPUT) {
            const int key = rawKeyEvent(l);
            if (key == 2) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
            if (key == 1) return DefWindowProcW(hwnd, msg, w, l); // a key press: swallowed
            handleRawMouse(l);
            return DefWindowProcW(hwnd, msg, w, l); // swallowed: the game does not see it
        }
        // Keyboard and text always go to ImGui; legacy mouse messages only while no raw input came.
        if (!isMouseMessage(msg) || !g_rawMouse) ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l);
        if (isInputMessage(msg)) return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
}

int keyFromName(const std::string& n) {
    for (const MenuKey& k : kMenuKeys)
        if (_stricmp(n.c_str(), k.name) == 0) return k.vk;
    return VK_INSERT;
}

bool init(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) return false;
    g_device->GetImmediateContext(&g_context);
    g_swapChain = sc;
    g_window = desc.OutputWindow;
    createRtv();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini next to the game
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    // Readable on big screens: Segoe UI scaled with the window height. Without it, ImGui's built-in
    // font at the same size (A-1: it used to stay at its 13 px default).
    // Names, chat and marker titles are the players' own text, so the font carries more than Latin:
    // Latin Extended, Greek and Cyrillic from Segoe UI, and the common Chinese characters from
    // Microsoft YaHei where Windows has it. Anything else still shows as "?".
    RECT rc{};
    GetClientRect(g_window, &rc);
    float px = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) / 55.f : 18.f;
    if (px < 16.f) px = 16.f;
    ImFont* font = nullptr;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        static ImVector<ImWchar> ranges; // the atlas reads it when it is built, on the first frame
        static const ImWchar kMore[] = {0x0100, 0x024F, 0x0370, 0x03FF, 0x2000, 0x206F, 0};
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        builder.AddRanges(kMore);
        builder.BuildRanges(&ranges);
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", px, nullptr, ranges.Data);
        if (font && GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", px, &merge, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
    }
    if (!font) {
        ImFontConfig fc;
        fc.SizePixels = px;
        io.Fonts->AddFontDefault(&fc);
    }
    menu::applyStyle();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
    MLOG("ui: overlay ready (%ldx%ld). Press %s in game for the MECMP menu.", rc.right - rc.left, rc.bottom - rc.top,
         config::get()->menuKey.c_str());
    return true;
}

HRESULT STDMETHODCALLTYPE presentHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_ready && !g_swapChain) g_ready = init(sc);
    // Ghosts move every frame, not per server tick. Normally written by the client replay updater
    // (on the game's thread, just before it reads them); from here only until that hook is in.
    if (g_ready && sc == g_swapChain && !game::hooks::clientUpdateHooked()) game::ghosts::renderFrame();
    if (g_ready && sc == g_swapChain && g_rtv) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (g_open && g_rawMouse) applyRawMouse(); // after the backend's own cursor read, so ours wins
        // While a gamepad binding is being recorded, the pad must not also click through the menu.
        ImGuiIO& io = ImGui::GetIO();
        if (gamepad::capturing()) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        ImGui::NewFrame();
        if (g_justOpened.exchange(false)) ImGui::SetWindowFocus("###mecmp");
        ImGui::GetIO().MouseDrawCursor = g_open.load();
        nametags::draw();
        markers::draw();
        radial::draw();
        if (const float a = game::teleport::fadeAlpha(); a > 0.f) // "Go to": the game's teleport behind a fade
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize,
                                                          ImGui::GetColorU32(ImVec4(0, 0, 0, a)));
        notices::draw();
        game::mumble::update();
        chat::draw();
        menu::draw(g_open.load());
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE resizeHook(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (sc == g_swapChain && g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    const HRESULT hr = g_origResize(sc, count, w, h, fmt, flags);
    if (sc == g_swapChain && g_ready) createRtv();
    return hr;
}
} // namespace

void setOnWindowDestroyed(void (*fn)()) { g_onDestroyed = fn; }

void toggleMenu() {
    const bool open = !g_open.load();
    if (open) radial::cancel(); // the menu takes the mouse
    if (open && g_window) {
        ClipCursor(nullptr);
        RECT rc{};
        GetClientRect(g_window, &rc);
        g_cursorX = (rc.right - rc.left) * 0.5f; // start in the middle of the screen
        g_cursorY = (rc.bottom - rc.top) * 0.5f;
        g_rawMouse = false;
        g_justOpened = true;
    }
    if (open) chat::close(); // the menu takes the keyboard
    if (!open) {
        g_keyCapture = false;
        gamepad::cancelCapture();
    }
    g_open = open;
}

const MenuKey* menuKeys(int& count) {
    count = static_cast<int>(sizeof(kMenuKeys) / sizeof(kMenuKeys[0]));
    return kMenuKeys;
}
void setMenuKey(const std::string& name) { g_toggleKey = keyFromName(name); }
bool setPingKey(const std::string& name) {
    if (name.empty()) {
        g_pingKey = 0;
        return true;
    }
    const int key = actionKeyFromName(name);
    if (key == 0 || key == g_toggleKey.load() || key == g_chatKey.load() || key == g_pingClearKey.load()) return false;
    g_pingKey = key;
    return true;
}

namespace {
// As actionKeyFromName, plus the mouse buttons a "clear markers" binding may use.
int clearKeyFromName(const std::string& n) {
    if (_stricmp(n.c_str(), "MOUSE3") == 0) return VK_MBUTTON;
    if (_stricmp(n.c_str(), "MOUSE4") == 0) return VK_XBUTTON1;
    if (_stricmp(n.c_str(), "MOUSE5") == 0) return VK_XBUTTON2;
    return actionKeyFromName(n);
}
} // namespace

bool setPingClearKey(const std::string& name) {
    if (name.empty()) {
        g_pingClearKey = 0;
        return true;
    }
    const int key = clearKeyFromName(name);
    if (key == 0 || key == g_toggleKey.load() || key == g_chatKey.load() || key == g_pingKey.load()) return false;
    g_pingClearKey = key;
    return true;
}

int pingClearVk() { return g_pingClearKey.load(); }
bool gameFocused() { return g_window && GetForegroundWindow() == g_window; }

std::string keyLabel(const std::string& name) {
    if (_stricmp(name.c_str(), "MOUSE3") == 0) return "Middle click";
    if (_stricmp(name.c_str(), "MOUSE4") == 0) return "Mouse 4";
    if (_stricmp(name.c_str(), "MOUSE5") == 0) return "Mouse 5";
    return name;
}
void setKeyCapture(bool on) { g_keyCapture = on; }

bool menuOpen() { return g_open.load(); }

int actionKeyFromName(const std::string& n) {
    if (n.size() == 1 && isalpha(static_cast<unsigned char>(n[0]))) return toupper(static_cast<unsigned char>(n[0]));
    if (_stricmp(n.c_str(), "ENTER") == 0) return VK_RETURN;
    for (const MenuKey& k : kMenuKeys)
        if (_stricmp(n.c_str(), k.name) == 0) return k.vk;
    return 0;
}

bool install() {
    g_toggleKey = keyFromName(config::get()->menuKey);
    const int chatKey = actionKeyFromName(config::get()->chatKey);
    g_chatKey = chatKey ? chatKey : 'T';
    // The marker key must not double as the menu or chat key (those win).
    const int pingKey = actionKeyFromName(config::get()->pingKey);
    g_pingKey = pingKey == g_toggleKey.load() || pingKey == g_chatKey.load() ? 0 : pingKey;
    setPingClearKey(config::get()->pingClearKey); // refused (so none) if it is one of the keys above
    gamepad::setBinding(gamepad::parse(config::get()->menuPad));
    gamepad::setBinding(gamepad::parse(config::get()->pingPad), gamepad::Action::Ping);
    gamepad::setBinding(gamepad::parse(config::get()->pingClearPad), gamepad::Action::PingClear);
    // A throwaway device + swap chain on a hidden window, only to find DXGI's swap-chain vtable.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MECMP_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr) || !sc) {
        MLOG("ui: could not create a probe swap chain (0x%08lx); no in-game menu", static_cast<unsigned long>(hr));
        DestroyWindow(hwnd);
        return false;
    }
    void** vtbl = *reinterpret_cast<void***>(sc);
    const uintptr_t presentSlot = reinterpret_cast<uintptr_t>(&vtbl[8]);
    const uintptr_t resizeSlot = reinterpret_cast<uintptr_t>(&vtbl[13]);
    uintptr_t oldPresent = 0, oldResize = 0;
    g_origPresent = reinterpret_cast<PresentFn>(vtbl[8]);
    g_origResize = reinterpret_cast<ResizeFn>(vtbl[13]);
    const bool ok = mem::patchPointer(presentSlot, reinterpret_cast<uintptr_t>(&presentHook), &oldPresent) &&
                    mem::patchPointer(resizeSlot, reinterpret_cast<uintptr_t>(&resizeHook), &oldResize);
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!ok) {
        MLOG("ui: could not hook Present; no in-game menu");
        return false;
    }
    return true;
}

} // namespace mecmp::ui
