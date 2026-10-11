#include "display.h"
#include "config.h"
#include "log.h"
#include "patch.h"

#include <dxgi.h>
#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace
{
constexpr uintptr_t kCreateThunk = 0x404D00;   // jmp sub_D9BB20 (device creation)
constexpr uintptr_t kCreateFn    = 0xD9BB20;
constexpr uintptr_t kResizeCall  = 0x8C33BC;   // call sub_D97D80 (settings change)
constexpr uintptr_t kResizeFn    = 0xD97D80;

struct DesktopMode
{
    DWORD width = 0, height = 0, hz = 0;
};

// The display mode Windows currently uses on the game's monitor.
bool QueryDesktop(IDXGIOutput* output, DesktopMode* out)
{
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc)))
        return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
        return false;
    out->width = dm.dmPelsWidth;
    out->height = dm.dmPelsHeight;
    out->hz = dm.dmDisplayFrequency;
    return out->hz > 1;
}

// Among the output's modes at this resolution and format, the one whose
// refresh rate is closest to `targetHz` (the higher one on a tie); with no
// target, the highest.
bool PickRate(IDXGIOutput* output, UINT width, UINT height, DXGI_FORMAT format, double targetHz,
              UINT* num, UINT* den)
{
    UINT n = 0;
    if (FAILED(output->GetDisplayModeList(format, 0, &n, nullptr)) || n == 0)
        return false;
    std::vector<DXGI_MODE_DESC> modes(n);
    if (FAILED(output->GetDisplayModeList(format, 0, &n, modes.data())))
        return false;
    bool found = false;
    double bestHz = 0.0, bestErr = 0.0;
    for (UINT i = 0; i < n; ++i)
    {
        const DXGI_MODE_DESC& m = modes[i];
        if (m.Width != width || m.Height != height || m.RefreshRate.Denominator == 0)
            continue;
        const double hz = static_cast<double>(m.RefreshRate.Numerator) / m.RefreshRate.Denominator;
        const double err = targetHz > 0.0 ? (hz > targetHz ? hz - targetHz : targetHz - hz) : -hz;
        if (!found || err < bestErr - 1e-6 || (err < bestErr + 1e-6 && hz > bestHz))
        {
            found = true;
            bestErr = err;
            bestHz = hz;
            *num = m.RefreshRate.Numerator;
            *den = m.RefreshRate.Denominator;
        }
    }
    return found;
}
void AdjustRefreshRate(uint32_t* params)
{
    const UINT width = params[3], height = params[4];
    const auto format = static_cast<DXGI_FORMAT>(params[5]);
    const double have = params[17] ? static_cast<double>(params[16]) / params[17] : 0.0;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)) &&
        SUCCEEDED(adapter->EnumOutputs(0, &output)))
    {
        DesktopMode desktop;
        const bool haveDesktop = QueryDesktop(output, &desktop);
        // RefreshRate in the INI wins; otherwise follow Windows.
        const double target = g_config.refreshRate > 0 ? g_config.refreshRate
                            : haveDesktop                ? static_cast<double>(desktop.hz)
                                                         : 0.0;
        UINT num = 0, den = 0;
        if (PickRate(output, width, height, format, target, &num, &den))
        {
            const double want = static_cast<double>(num) / den;
            if (num != params[16] || den != params[17])
            {
                params[16] = num;
                params[17] = den;
            }
            if (haveDesktop && desktop.width == width && desktop.height == height &&
                g_config.refreshRate <= 0)
                LOG_INFO("fullscreen %ux%u @ %.3f Hz (game asked for %.3f): same as the "
                         "desktop, no display mode change",
                         width, height, want, have);
            else
                LOG_INFO("fullscreen %ux%u @ %.3f Hz (game asked for %.3f, desktop %lux%lu @ %lu)",
                         width, height, want, have, desktop.width, desktop.height, desktop.hz);
        }
        else
        {
            LOG_WARN("display mode %ux%u not in the monitor's mode list; refresh rate left at "
                     "%.3f Hz",
                     width, height, have);
        }
    }
    if (output)
        output->Release();
    if (adapter)
        adapter->Release();
    factory->Release();
}

// A borderless window over the whole monitor the game window is on.
void MakeBorderless(HWND hwnd)
{
    if (!IsWindow(hwnd))
        return;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        return;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
    SetWindowLongW(hwnd, GWL_EXSTYLE,
                   exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                               WS_EX_WINDOWEDGE));
    const RECT& r = mi.rcMonitor;
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

bool g_loggedBorderless = false;
} // namespace

uintptr_t g_displayCreateFn = kCreateFn;
uintptr_t g_displayResizeFn = kResizeFn;

// Settings block: output window at +0x54, the fullscreen flag at byte +0x61
// (the swap chain is created Windowed when it is 0).
extern "C" void __cdecl AdjustDisplay(uint32_t* params)
{
    if (!params)
        return;
    if (!g_config.borderless)
    {
        AdjustRefreshRate(params);
        return;
    }
    reinterpret_cast<uint8_t*>(params)[0x61] = 0;
    MakeBorderless(reinterpret_cast<HWND>(static_cast<uintptr_t>(params[21])));
    if (!g_loggedBorderless)
    {
        g_loggedBorderless = true;
        LOG_INFO("borderless window, %ux%u rendered", params[3], params[4]);
    }
}

void OnSwapChain(IDXGISwapChain* swapChain)
{
    if (!g_config.borderless)
        return;
    // DXGI's own Alt+Enter would switch to exclusive fullscreen.
    DXGI_SWAP_CHAIN_DESC desc{};
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(swapChain->GetDesc(&desc)) &&
        SUCCEEDED(swapChain->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
    {
        factory->MakeWindowAssociation(desc.OutputWindow, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }
    BOOL fullscreen = FALSE;
    if (SUCCEEDED(swapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen)
    {
        swapChain->SetFullscreenState(FALSE, nullptr);
        MakeBorderless(desc.OutputWindow);
    }
}

// Both targets take the settings block as their first stack argument; the
// creation function also takes arguments in edi / esi, so every register is
// preserved around the call.
extern "C" __declspec(naked) void DisplayCreateThunk()
{
    __asm
    {
        pushad
        push dword ptr [esp + 36]
        call AdjustDisplay
        add esp, 4
        popad
        mov eax, g_displayCreateFn
        jmp eax
    }
}

extern "C" __declspec(naked) void DisplayResizeThunk()
{
    __asm
    {
        pushad
        push dword ptr [esp + 36]
        call AdjustDisplay
        add esp, 4
        popad
        mov eax, g_displayResizeFn
        jmp eax
    }
}

bool DisplaySitesOk()
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(kCreateThunk);
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return p[0] == 0xE9 && kCreateThunk + 5 + rel == kCreateFn &&
           patch::CallTarget(kResizeCall) == kResizeFn;
}

void InstallDisplay()
{
    patch::Jump(kCreateThunk, reinterpret_cast<const void*>(&DisplayCreateThunk));
    patch::Branch(kResizeCall, 0xE8, reinterpret_cast<const void*>(&DisplayResizeThunk));
}
