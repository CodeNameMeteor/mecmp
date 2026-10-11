#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "ui/overlay.h"

#include <atomic>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "core/config.h"
#include "core/log.h"
#include "core/mem.h"
#include "imgui.h"
#include "ui/menu.h"

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
int g_toggleKey = VK_INSERT;

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
    for (int i = 0; i < 3; ++i) {
        // A click that went down and up between two frames: press this frame, release the next.
        if (down & (1u << i)) io.AddMouseButtonEvent(i, true);
        if (up & (1u << i)) {
            if (down & (1u << i)) g_btnUp |= (1u << i);
            else io.AddMouseButtonEvent(i, false);
        }
    }
    if (const int w = g_wheel.exchange(0)) io.AddMouseWheelEvent(0.f, static_cast<float>(w) / WHEEL_DELTA);
}

bool isMouseMessage(UINT msg) { return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST; }

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY && hwnd == g_window) {
        if (auto fn = g_onDestroyed.exchange(nullptr)) fn();
    }
    if (msg == WM_KEYDOWN && static_cast<int>(w) == g_toggleKey && !(l & (1 << 30))) {
        const bool open = !g_open;
        g_open = open;
        if (open) {
            ClipCursor(nullptr);
            RECT rc{};
            GetClientRect(g_window, &rc);
            g_cursorX = (rc.right - rc.left) * 0.5f; // start in the middle of the screen
            g_cursorY = (rc.bottom - rc.top) * 0.5f;
            g_rawMouse = false;
        }
        return 0;
    }
    if (g_open && g_ready) {
        if (msg == WM_INPUT) {
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
    if (n.size() >= 2 && (n[0] == 'F' || n[0] == 'f')) {
        const int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    if (_stricmp(n.c_str(), "HOME") == 0) return VK_HOME;
    if (_stricmp(n.c_str(), "END") == 0) return VK_END;
    if (_stricmp(n.c_str(), "DELETE") == 0) return VK_DELETE;
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Readable on big screens: Segoe UI scaled with the window height.
    RECT rc{};
    GetClientRect(g_window, &rc);
    const float px = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) / 55.f : 18.f;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", px < 16.f ? 16.f : px);
    menu::applyStyle();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
    MLOG("ui: overlay ready (%ldx%ld). Press %s in game for the MECMP menu.", rc.right - rc.left, rc.bottom - rc.top,
         config::get().menuKey.c_str());
    return true;
}

HRESULT STDMETHODCALLTYPE presentHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_ready && !g_swapChain) g_ready = init(sc);
    if (g_ready && sc == g_swapChain && g_rtv) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (g_open && g_rawMouse) applyRawMouse(); // after the backend's own cursor read, so ours wins
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = g_open.load();
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

bool menuOpen() { return g_open.load(); }

bool install() {
    g_toggleKey = keyFromName(config::get().menuKey);
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
