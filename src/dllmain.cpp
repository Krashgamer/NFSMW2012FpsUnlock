#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "barrier.h"
#include "config.h"
#include "crashguard.h"
#include "framelatency.h"
#include "guards.h"
#include "log.h"
#include "pacer.h"
#include "streamsync.h"
#include "display.h"
#include "version.h"
#include "blend/frame.h"
#include "blend/objects.h"
#include "blend/parts.h"
#include "blend/player.h"
#include "blend/readouts.h"

namespace
{
const char kTitle[] = "MW2012 FPS Unlock";

// NFS13.exe v1.5 (13,684,056 bytes): entry point 0x11DD830 (Steam / Origin)
// or 0xEE279B (disc). The same check NFSMW2012HDEffects uses.
bool IsSupportedExe(uintptr_t* entryOut)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uintptr_t entry = nt->OptionalHeader.AddressOfEntryPoint + 0x400000;
    *entryOut = entry;
    return entry == 0x11DD830 || entry == 0xEE279B;
}

// The exe decrypts its code at startup, and ASI loaders may load plugins
// before that has happened. Wait until the pacer's code is readable (60 s).
bool WaitForDecryptedCode()
{
    for (int i = 0; i < 6000; ++i)
    {
        if (PacerSiteOk())
            return true;
        Sleep(10);
    }
    return false;
}

const char* Result(bool ok) { return ok ? "ok" : "FAILED"; }

DWORD WINAPI StatsThread(LPVOID)
{
    long frames = FrameCount(), ticks = TickCount();
    for (;;)
    {
        Sleep(60000);
        const long f = FrameCount(), t = TickCount();
        LOG_VERBOSE("last 60 s: %.1f fps, %.1f ticks/s, %ld emulated faults total",
                    (f - frames) / 60.0, (t - ticks) / 60.0, EmulatedFaultCount());
        frames = f;
        ticks = t;
    }
}

void Install()
{
    if (!WaitForDecryptedCode())
    {
        LOG_ERROR("the game's code did not appear within 60 s; nothing was patched");
        return;
    }
    // First, so it is in place before the game creates its device.
    if (DisplaySitesOk())
    {
        InstallDisplay();
        LOG_INFO("display mode: ok");
    }
    else
    {
        LOG_WARN("display mode: unexpected code, left to the game");
    }

    // Every site the unlock depends on is checked before anything is written:
    // the unlock is all or nothing.
    if (!BarrierSitesOk() || !frame::SitesOk())
    {
        LOG_ERROR("unexpected code at a patch site (barrier=%s frame=%s); the frame rate "
                  "stays locked",
                  Result(BarrierSitesOk()), Result(frame::SitesOk()));
        MessageBoxA(nullptr,
                    "This NFS13.exe does not match the expected code.\n"
                    "The frame rate unlock was NOT applied.",
                    kTitle, MB_ICONWARNING);
        return;
    }
    if (!InstallBarrierHooks())
    {
        LOG_ERROR("worker barrier could not be installed; the frame rate stays locked");
        return;
    }
    LOG_INFO("worker barrier: ok");

    StartFrameLatencyLimit();
    LOG_INFO("slot bounds guards: %d/4", InstallSlotBoundsGuards());
    LOG_INFO("command replay guards: %d", InstallReplayGuards());

    InstallPacer();
    LOG_INFO("streaming schedule: %s", Result(InstallStreamSync()));
    LOG_INFO("frame pacer: ok (%s)", g_config.targetFps ? "capped" : "uncapped");

    frame::Install();
    LOG_INFO("frame hooks: ok");
    LOG_INFO("effect requests: %s", Result(parts::InstallEffectRequests()));
    LOG_INFO("readout nodes: %s", Result(readouts::InstallNodeHook()));
    LOG_INFO("physics-mode wheels: %s", Result(parts::InstallPhysicsWheels()));
    LOG_INFO("prop pieces: %s", Result(parts::InstallPropPieces()));
    LOG_INFO("entity getters: %d/11", objects::InstallGetterHooks());
    LOG_INFO("player registry: %s", Result(player::InstallRegistryHook()));

    InstallCrashGuard();
    LOG_INFO("crash guard: ok");

    if (LogEnabled(LogLevel::Verbose))
    {
        HANDLE h = CreateThread(nullptr, 0, &StatsThread, nullptr, 0, nullptr);
        if (h)
            CloseHandle(h);
    }
}

DWORD WINAPI InitThread(LPVOID)
{
    LoadConfig();
    LogOpen(g_config.logLevel);
    LOG_INFO("MW2012 FPS Unlock " MW_VERSION_STRING " (built " __DATE__ ")");

    uintptr_t entry = 0;
    if (!IsSupportedExe(&entry))
    {
        LOG_ERROR("unsupported NFS13.exe (entry point 0x%08X); nothing was patched",
                  static_cast<unsigned>(entry));
        MessageBoxA(nullptr,
                    "This NFS13.exe is not supported.\n"
                    "Please use the v1.5 exe (13,684,056 bytes).",
                    kTitle, MB_ICONERROR);
        return 0;
    }
    LOG_INFO("settings: TargetFPS=%d LoadingScreenCap=%d Borderless=%d RefreshRate=%d "
             "VsyncInterval=%d LogLevel=%d",
             g_config.targetFps, g_config.capLoading ? 1 : 0, g_config.borderless ? 1 : 0,
             g_config.refreshRate, g_config.vsyncInterval, g_config.logLevel);

    Install();
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // Nothing is patched from DllMain: the loader lock is held, and the
        // game's code may not be decrypted yet.
        HANDLE h = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
        if (h)
            CloseHandle(h);
    }
    return TRUE;
}
