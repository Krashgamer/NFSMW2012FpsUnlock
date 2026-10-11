#include "framelatency.h"
#include "display.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <cstdint>

#pragma comment(lib, "dxguid.lib")

namespace
{
constexpr uintptr_t kSwapChainSlot = 0x11ACDE4;   // the game's IDXGISwapChain*
constexpr UINT      kFrameLatency  = 1;

bool InDxgi(uintptr_t p)
{
    HMODULE m = GetModuleHandleA("dxgi.dll");
    if (!m || !p)
        return false;
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

// True once the latency is set.
bool TrySet()
{
    if (!GetModuleHandleA("dxgi.dll"))
        return false;                            // no device yet
    const uintptr_t obj = *reinterpret_cast<volatile uintptr_t*>(kSwapChainSlot);
    if (obj < 0x10000 || (obj & 3) != 0)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(obj), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT)
        return false;
    // Anything dxgi.dll implements has its vtable inside dxgi.dll.
    if (!InDxgi(*reinterpret_cast<volatile uintptr_t*>(obj)))
        return false;

    bool done = false;
    __try
    {
        auto* unk = reinterpret_cast<IUnknown*>(obj);
        IDXGISwapChain* sc = nullptr;
        if (FAILED(unk->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&sc))) ||
            !sc)
            return false;
        IDXGIDevice1* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dev))) &&
            dev)
        {
            UINT before = 0, after = 0;
            dev->GetMaximumFrameLatency(&before);
            dev->SetMaximumFrameLatency(kFrameLatency);
            dev->GetMaximumFrameLatency(&after);
            dev->Release();
            LOG_INFO("DXGI maximum frame latency %u -> %u", before, after);
            done = true;
        }
        if (done)
            OnSwapChain(sc);
        sc->Release();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return done;
}

DWORD WINAPI LatencyThread(LPVOID)
{
    for (int i = 0; i < 120; ++i)
    {
        if (TrySet())
            return 0;
        Sleep(1000);
    }
    LOG_WARN("DXGI frame latency not set: no swap chain found within 120 s");
    return 0;
}
} // namespace

void StartFrameLatencyLimit()
{
    HANDLE h = CreateThread(nullptr, 0, &LatencyThread, nullptr, 0, nullptr);
    if (h)
        CloseHandle(h);
}
