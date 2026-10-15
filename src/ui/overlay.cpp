#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "ui/overlay.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "core/config.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/prof.h"
#include "game/ghosts.h"
#include "game/hooks.h"
#include "game/ping_point.h"
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
#include "ui/summon.h"

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
std::atomic<int> g_reactKey{0}; // the quick answers key; 0 = none
std::atomic<int> g_altKey{0}, g_hideKey{0}, g_prevKey{0}, g_nextKey{0}; // the other keys; 0 = none
std::atomic<int> g_summonKey{0}, g_summonAcceptKey{0}, g_summonDeclineKey{0}; // summon (ui/summon.h); 0 = none
std::atomic<bool> g_hidden{false};       // the overlay is hidden (see overlayHidden)
std::atomic<DWORD> g_hiddenNoteAt{0};    // when it was last switched: a short note says so
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

// Window messages for ImGui's own input (keyboard, text, chat, the menu's keys): the Win32 backend
// adds them to ImGui's input queue, which the render thread drains in NewFrame. So the window thread
// only queues them, and the render thread passes them on before the backend's NewFrame.
struct ImGuiMsg { HWND hwnd; UINT msg; WPARAM w; LPARAM l; };
std::mutex g_imguiMsgM;
std::vector<ImGuiMsg> g_imguiMsgs; // guarded by g_imguiMsgM
constexpr size_t kMaxImGuiMsgs = 256; // a backlog this long means the frame loop is stuck; newer messages are dropped

void queueImGui(HWND hwnd, UINT msg, WPARAM w, LPARAM l) { // window thread
    std::lock_guard<std::mutex> lock(g_imguiMsgM);
    if (g_imguiMsgs.size() < kMaxImGuiMsgs) g_imguiMsgs.push_back({hwnd, msg, w, l});
}

void deliverImGuiMsgs() { // render thread, before the backend's NewFrame
    std::vector<ImGuiMsg> msgs;
    {
        std::lock_guard<std::mutex> lock(g_imguiMsgM);
        msgs.swap(g_imguiMsgs);
    }
    for (const ImGuiMsg& m : msgs) ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.w, m.l);
}
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

// A raw keyboard event: its virtual key and whether it is a press (not a release). False if it is
// not the keyboard.
bool rawKey(LPARAM l, int& vk, bool& press) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return false;
    if (ri.header.dwType != RIM_TYPEKEYBOARD) return false;
    vk = ri.data.keyboard.VKey;
    press = !(ri.data.keyboard.Flags & RI_KEY_BREAK);
    return true;
}

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
    if (b & RI_MOUSE_RIGHT_BUTTON_DOWN) radial::dismiss();
    if (b & RI_MOUSE_WHEEL) radial::switchWheel(static_cast<SHORT>(m.usButtonData) > 0 ? -1 : 1);
    return true;
}

// Keys bound to the mouse's middle and side buttons do not come as key messages, so they are asked
// for every frame (render thread). The marker and answer keys start the wheel as a key would; its
// release is noticed by the wheel itself. The hide key is not here: pollHideKey asks for it, any key.
void pollMouseBinds() {
    static bool was[6] = {};
    std::atomic<int>* const binds[6] = {&g_pingKey, &g_reactKey, &g_altKey, &g_hideKey, &g_prevKey, &g_nextKey};
    const bool canAct = gameFocused() && !g_open.load() && !chat::isOpen() && g_ready && !game::hooks::gameMenuOpen();
    for (int i = 0; i < 6; ++i) {
        if (i == 3) continue;
        const int vk = binds[i]->load();
        const bool mouse = vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
        const bool now = mouse && canAct && (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool pressed = now && !was[i];
        was[i] = now;
        if (!pressed) continue;
        switch (i) {
        case 0: radial::keyDown(vk); break;
        case 1: radial::keyDown(vk, radial::Mode::React); break;
        case 2: game::ping::requestAlt(); break;
        case 4: radial::switchWheel(-1); break;
        default: radial::switchWheel(1); break;
        }
    }
}

constexpr DWORD kHideHoldMs = 1000; // how long the hide key is held to hide or show (a stray press must do nothing)
float g_hideProgress = 0.f;         // how far that hold is, 0..1 (render thread only)

// The hide key (render thread, every frame). Hiding and showing again both take a hold of kHideHoldMs.
// Asked for here rather than through key messages, so it is the same for a mouse button.
void pollHideKey() {
    static bool held = false, done = false;
    static DWORD since = 0;
    g_hideProgress = 0.f;
    const int vk = g_hideKey.load();
    const bool now = vk != 0 && gameFocused() && !g_open.load() && !chat::isOpen() && g_ready &&
                     (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!now) {
        held = done = false;
        return;
    }
    if (!held) {
        held = true;
        since = GetTickCount();
    }
    if (done) return;
    const DWORD elapsed = GetTickCount() - since;
    if (elapsed >= kHideHoldMs) {
        done = true;
        setOverlayHidden(!g_hidden.load());
        return;
    }
    g_hideProgress = static_cast<float>(elapsed) / static_cast<float>(kHideHoldMs);
}

// A pill of text at the top of the screen; `progress` above 0 fills a bar along its bottom edge.
void drawTopPill(const std::string& text, float progress) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 a((screen.x - ts.x) * 0.5f, screen.y * 0.04f);
    const ImVec2 p0(a.x - size * 0.5f, a.y - size * 0.25f), p1(a.x + ts.x + size * 0.5f, a.y + ts.y + size * 0.25f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(12, 12, 18, 190), size * 0.3f);
    if (progress > 0.f) {
        const float inset = size * 0.3f, bar = std::max(2.f, size * 0.12f);
        const float w = (p1.x - p0.x - 2.f * inset) * std::min(progress, 1.f);
        dl->AddRectFilled(ImVec2(p0.x + inset, p1.y - bar - 1.f), ImVec2(p0.x + inset + w, p1.y - 1.f),
                          IM_COL32(255, 110, 70, 230), bar * 0.5f);
    }
    dl->AddText(font, size, a, IM_COL32(255, 255, 255, 255), text.c_str());
}

// While the hide key is held: how long to go. A moment after the overlay was hidden or shown: a line
// at the top says which, and how to undo it.
void drawHiddenNote() {
    const std::string key = config::get()->hideKey;
    if (g_hideProgress > 0.1f) {
        drawTopPill("Keep holding " + keyLabel(key) + (g_hidden.load() ? " to show the overlay" : " to hide the overlay"),
                    g_hideProgress);
        return;
    }
    const DWORD at = g_hiddenNoteAt.load();
    if (!at || GetTickCount() - at > 2500) return;
    std::string text = g_hidden.load() ? "Overlay hidden" : "Overlay shown";
    if (g_hidden.load() && !key.empty()) text += " (" + keyLabel(key) + " shows it again)";
    drawTopPill(text, 0.f);
}

// The keys the open wheel uses itself: the digits, the arrows and the previous / next wheel keys. While
// the marker key is down the game does not get their presses (releases still reach it, as with the
// chat, so no key is ever left "down" in the game).
bool wheelKey(int vk) {
    if (vk >= '1' && vk <= '9') return true;
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return true;
    if (vk == VK_LEFT || vk == VK_RIGHT) return true;
    return (g_prevKey.load() != 0 && vk == g_prevKey.load()) || (g_nextKey.load() != 0 && vk == g_nextKey.load());
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    // Alt+F4 always reaches the game's window: the menu and the chat would otherwise swallow it.
    if (msg == WM_SYSKEYDOWN && static_cast<int>(w) == VK_F4) return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
    const bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (keyUp && g_ready) queueImGui(hwnd, msg, w, l);
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
    if (msg == WM_KEYUP && ((g_pingKey.load() != 0 && static_cast<int>(w) == g_pingKey.load()) ||
                            (g_reactKey.load() != 0 && static_cast<int>(w) == g_reactKey.load())))
        radial::keyUp(static_cast<int>(w));
    if (radial::engaged() && msg == WM_KEYDOWN && !(l & (1 << 30))) {
        const int key = static_cast<int>(w);
        if (key >= '1' && key <= '9') radial::digit(key - '0');
        else if (key >= VK_NUMPAD1 && key <= VK_NUMPAD9) radial::digit(key - VK_NUMPAD0);
        else if (key == VK_LEFT || (g_prevKey.load() && key == g_prevKey.load())) radial::switchWheel(-1);
        else if (key == VK_RIGHT || (g_nextKey.load() && key == g_nextKey.load())) radial::switchWheel(1);
    }
    if (radial::engaged()) {
        if (msg == WM_KEYDOWN && wheelKey(static_cast<int>(w))) return 0;
        int vk = 0;
        bool press = false;
        if (msg == WM_INPUT && rawKey(l, vk, press) && press && wheelKey(vk)) return DefWindowProcW(hwnd, msg, w, l);
    }
    if (radial::open()) {
        if (msg == WM_INPUT && radialRawMouse(l)) return DefWindowProcW(hwnd, msg, w, l);
        if (isMouseMessage(msg)) {
            if (!g_radialRaw) { // no raw mouse input here: the legacy messages are all there is
                if (msg == WM_LBUTTONDOWN) radial::click();
                else if (msg == WM_RBUTTONDOWN) radial::dismiss();
                else if (msg == WM_MOUSEWHEEL) radial::switchWheel(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1);
            }
            return 0;
        }
    }
    // Summon: while a summon waits, its accept key (and the decline key, also while "Go back?" shows)
    // belongs to the panel. Presses stay away from the game; releases reach it, so no key is ever left
    // "down" there (as with the wheel's keys).
    if (!g_open.load() && !chat::isOpen() && g_ready) {
        const int accept = g_summonAcceptKey.load(), decline = g_summonDeclineKey.load();
        // Not while a game menu or the marker wheel uses the keys: those are the game's or the wheel's then.
        const bool unclaimed = !game::hooks::gameMenuOpen() && !radial::engaged();
        auto taken = [&](int vk) {
            return unclaimed && vk != 0 && ((vk == accept && summon::takesAcceptKey()) || (vk == decline && summon::takesDeclineKey()));
        };
        if (msg == WM_KEYDOWN && taken(static_cast<int>(w))) return 0;
        int vk = 0;
        bool press = false;
        if (msg == WM_INPUT && rawKey(l, vk, press) && press && taken(vk)) return DefWindowProcW(hwnd, msg, w, l);
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
    // The quick answers key: the same, for the answers wheel (it needs a marker under the crosshair).
    if (msg == WM_KEYDOWN && !g_open && !chat::isOpen() && g_ready && g_reactKey.load() != 0 &&
        static_cast<int>(w) == g_reactKey.load() && !(l & (1 << 30)) && !game::hooks::gameMenuOpen())
        radial::keyDown(g_reactKey.load(), radial::Mode::React);
    // The other-way key: a short press places a marker at the other kind of point.
    if (msg == WM_KEYDOWN && !g_open && !chat::isOpen() && g_ready && g_altKey.load() != 0 &&
        static_cast<int>(w) == g_altKey.load() && !(l & (1 << 30)) && !game::hooks::gameMenuOpen())
        game::ping::requestAlt();
    // (The hide key is polled every frame: pollHideKey.)
    if (chat::isOpen() && g_ready && !g_open) {
        if (msg == WM_KEYDOWN && w == VK_ESCAPE) { chat::close(); return 0; }
        if (msg == WM_CHAR && g_eatChar) { g_eatChar = false; return 0; }
        // Key presses go to the chat line only. Key releases and the mouse still reach the game, so a
        // key held when the chat opened is released there (Faith does not keep running).
        if (msg == WM_INPUT && isRawKeyPress(l)) return DefWindowProcW(hwnd, msg, w, l);
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR || msg == WM_SYSCHAR) {
            queueImGui(hwnd, msg, w, l);
            return 0;
        }
    }
    // A pad plugged in later: let the backend look for it again (it only sets a flag).
    if (msg == WM_DEVICECHANGE && g_ready) queueImGui(hwnd, msg, w, l);
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
        if (!isMouseMessage(msg) || !g_rawMouse) queueImGui(hwnd, msg, w, l);
        if (isInputMessage(msg)) return 0;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, w, l);
}

int keyFromName(const std::string& n) {
    for (const MenuKey& k : kMenuKeys)
        if (_stricmp(n.c_str(), k.name) == 0) return k.vk;
    return VK_INSERT;
}

// Font size and UI scale. The size follows the game window's height (so a window that starts at
// 1280x720 and then goes full screen is not left with 720p text) times the user's ui_scale. The atlas
// is baked at g_bakedPx; small differences are covered by FontGlobalScale, and a bigger one rebuilds
// the atlas once the wanted size has held still for a moment (a slider drag does not rebuild per step).
float g_bakedPx = 0.f;
float g_wantPx = 0.f;
DWORD g_wantSince = 0;

float targetFontPx() {
    RECT rc{};
    float h = 0.f;
    if (g_window && GetClientRect(g_window, &rc)) h = static_cast<float>(rc.bottom - rc.top);
    if (h <= 0.f && g_wantPx > 0.f) return g_wantPx; // minimised: keep the size it had
    float px = h > 0.f ? h / 55.f : 18.f;
    if (px < 16.f) px = 16.f;
    px *= std::clamp(config::get()->uiScale, 0.5f, 2.f);
    return std::clamp(px, 8.f, 96.f);
}

// Segoe UI at `px`. Names, chat and marker titles are the players' own text, so the font carries more
// than Latin: Latin Extended, Greek and Cyrillic from Segoe UI, and the common Chinese characters from
// Microsoft YaHei where Windows has it. Anything else still shows as "?".
void buildFonts(float px) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFont* font = nullptr;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        static ImVector<ImWchar> ranges; // the atlas reads it when it is built, on the next frame
        static const ImWchar kMore[] = {0x0100, 0x024F, 0x0370, 0x03FF, 0x2000, 0x206F, 0};
        ranges.clear();
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
    g_bakedPx = px;
    g_wantPx = px;
    g_wantSince = GetTickCount();
}

// Render thread, before the backend's NewFrame (which creates the font texture again after a rebuild).
void updateFontScale() {
    const float want = targetFontPx();
    const DWORD now = GetTickCount();
    if (want != g_wantPx) {
        g_wantPx = want;
        g_wantSince = now;
    }
    const float ratio = want / g_bakedPx;
    if ((ratio < 0.8f || ratio > 1.25f) && now - g_wantSince > 400) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        buildFonts(want);
        MLOG("ui: font rebuilt at %.0f px (ui_scale %.2f)", want, config::get()->uiScale);
    }
    ImGui::GetIO().FontGlobalScale = want / g_bakedPx;
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
    RECT rc{};
    GetClientRect(g_window, &rc);
    buildFonts(targetFontPx());
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
        prof::Scope scope(prof::Overlay);
        updateFontScale();
        ImGui_ImplDX11_NewFrame();
        deliverImGuiMsgs();
        ImGui_ImplWin32_NewFrame();
        if (g_open && g_rawMouse) applyRawMouse(); // after the backend's own cursor read, so ours wins
        // While a gamepad binding is being recorded, the pad must not also click through the menu.
        ImGuiIO& io = ImGui::GetIO();
        if (gamepad::capturing()) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        ImGui::NewFrame();
        if (g_justOpened.exchange(false)) ImGui::SetWindowFocus("###mecmp");
        ImGui::GetIO().MouseDrawCursor = g_open.load();
        pollMouseBinds();
        pollHideKey();
        {
            // Summon (ui/summon.h): held keys are read here, every frame, so tap and hold are told apart.
            auto down = [](int vk) { return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; };
            summon::Input in;
            in.canAct = gameFocused() && !g_open.load() && !chat::isOpen() && !game::hooks::gameMenuOpen() && !radial::engaged();
            in.sendDown = down(g_summonKey.load());
            in.acceptDown = down(g_summonAcceptKey.load()) || gamepad::held(gamepad::Action::SummonAccept);
            in.declineDown = down(g_summonDeclineKey.load()) || gamepad::held(gamepad::Action::SummonDecline);
            summon::update(in);
            gamepad::setSummonHidden(summon::takesAcceptKey(), summon::takesDeclineKey());
        }
        nametags::draw();
        markers::draw();
        radial::draw();
        drawHiddenNote();
        if (const float a = game::teleport::fadeAlpha(); a > 0.f) // "Go to": the game's teleport behind a fade
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize,
                                                          ImGui::GetColorU32(ImVec4(0, 0, 0, a)));
        notices::draw();
        summon::draw();
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
namespace {
// Every key the mod listens to besides the menu key. A key does one job.
std::atomic<int>* const kBound[] = {&g_pingKey, &g_pingClearKey, &g_chatKey, &g_reactKey, &g_altKey,
                                    &g_hideKey, &g_prevKey,      &g_nextKey, &g_summonKey, &g_summonAcceptKey,
                                    &g_summonDeclineKey};

// A key name from mecmp.ini. `mouse`: false = the keyboard only, true = also MOUSE3 (the middle
// button), MOUSE4 and MOUSE5 (the side buttons).
int bindFromName(const std::string& n, bool mouse) {
    if (mouse && _stricmp(n.c_str(), "MOUSE3") == 0) return VK_MBUTTON;
    if (mouse && _stricmp(n.c_str(), "MOUSE4") == 0) return VK_XBUTTON1;
    if (mouse && _stricmp(n.c_str(), "MOUSE5") == 0) return VK_XBUTTON2;
    return actionKeyFromName(n);
}

// A refused key from mecmp.ini: logged, so that a key that does nothing has a reason in the log.
void noteRefused(bool ok, const char* what, const std::string& name) {
    if (!ok && !name.empty()) MLOG("config: %s '%s' is already used by another action; it is off", what, name.c_str());
}

// A gamepad combination from mecmp.ini: refused (and logged) if another action has exactly these buttons.
void setPadBinding(const std::string& combo, gamepad::Action action, const char* what) {
    const uint16_t bits = gamepad::parse(combo);
    if (bits && !gamepad::bindingFree(bits, action)) {
        MLOG("config: %s '%s' is already used by another action; it is off", what, combo.c_str());
        gamepad::setBinding(0, action);
        return;
    }
    gamepad::setBinding(bits, action);
}

bool setBind(std::atomic<int>& slot, const std::string& name, bool mouse) {
    if (name.empty()) {
        slot = 0;
        return true;
    }
    const int key = bindFromName(name, mouse);
    if (key == 0 || key == g_toggleKey.load()) return false;
    for (const std::atomic<int>* other : kBound)
        if (other != &slot && other->load() == key) return false;
    slot = key;
    return true;
}
} // namespace

// The menu key is one of the keys the mod listens to, so it may not be one of the others either.
bool setMenuKey(const std::string& name) {
    const int key = actionKeyFromName(name);
    if (!key) return false;
    for (const std::atomic<int>* other : kBound)
        if (other->load() == key) return false;
    g_toggleKey = key;
    return true;
}
bool setPingKey(const std::string& name) { return setBind(g_pingKey, name, true); }
bool setPingClearKey(const std::string& name) { return setBind(g_pingClearKey, name, true); }
bool setChatKey(const std::string& name) { return setBind(g_chatKey, name, false); }
bool setReactKey(const std::string& name) { return setBind(g_reactKey, name, true); }
bool setAltKey(const std::string& name) { return setBind(g_altKey, name, true); }
bool setHideKey(const std::string& name) { return setBind(g_hideKey, name, true); }
bool setWheelPrevKey(const std::string& name) { return setBind(g_prevKey, name, true); }
bool setWheelNextKey(const std::string& name) { return setBind(g_nextKey, name, true); }
bool setSummonKey(const std::string& name) { return setBind(g_summonKey, name, true); }
bool setSummonAcceptKey(const std::string& name) { return setBind(g_summonAcceptKey, name, true); }
bool setSummonDeclineKey(const std::string& name) { return setBind(g_summonDeclineKey, name, true); }

bool middleButtonIsABind() {
    for (const std::atomic<int>* k : kBound)
        if (k != &g_pingClearKey && k->load() == VK_MBUTTON) return true;
    return false;
}

bool overlayHidden() { return g_hidden.load(); }
void setOverlayHidden(bool hidden) {
    g_hidden = hidden;
    g_hiddenNoteAt = GetTickCount() | 1; // never 0 (that means "never switched")
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
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return n[0]; // the number row (VK_0..VK_9 are the characters)
    if (_stricmp(n.c_str(), "ENTER") == 0) return VK_RETURN;
    for (const MenuKey& k : kMenuKeys)
        if (_stricmp(n.c_str(), k.name) == 0) return k.vk;
    return 0;
}

bool install() {
    g_toggleKey = keyFromName(config::get()->menuKey);
    g_chatKey = actionKeyFromName(config::get()->chatKey); // empty = no chat key
    // The marker key must not double as the menu or chat key (those win).
    const int pingKey = actionKeyFromName(config::get()->pingKey);
    g_pingKey = pingKey == g_toggleKey.load() || pingKey == g_chatKey.load() ? 0 : pingKey;
    // A key another action already has is refused and stays off; the log says so.
    noteRefused(setPingClearKey(config::get()->pingClearKey), "ping_clear_key", config::get()->pingClearKey);
    noteRefused(setReactKey(config::get()->reactKey), "react_key", config::get()->reactKey);
    noteRefused(setAltKey(config::get()->altKey), "ping_alt_key", config::get()->altKey);
    noteRefused(setHideKey(config::get()->hideKey), "hide_key", config::get()->hideKey);
    noteRefused(setWheelPrevKey(config::get()->wheelPrevKey), "wheel_prev_key", config::get()->wheelPrevKey);
    noteRefused(setWheelNextKey(config::get()->wheelNextKey), "wheel_next_key", config::get()->wheelNextKey);
    noteRefused(setSummonKey(config::get()->summonKey), "summon_key", config::get()->summonKey);
    noteRefused(setSummonAcceptKey(config::get()->summonAcceptKey), "summon_accept_key", config::get()->summonAcceptKey);
    noteRefused(setSummonDeclineKey(config::get()->summonDeclineKey), "summon_decline_key", config::get()->summonDeclineKey);
    // Gamepad combinations: the menu's first, so it wins over an identical one.
    setPadBinding(config::get()->menuPad, gamepad::Action::Menu, "menu_pad");
    setPadBinding(config::get()->pingPad, gamepad::Action::Ping, "ping_pad");
    setPadBinding(config::get()->pingClearPad, gamepad::Action::PingClear, "ping_clear_pad");
    setPadBinding(config::get()->reactPad, gamepad::Action::React, "react_pad");
    setPadBinding(config::get()->summonAcceptPad, gamepad::Action::SummonAccept, "summon_accept_pad");
    setPadBinding(config::get()->summonDeclinePad, gamepad::Action::SummonDecline, "summon_decline_pad");
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
