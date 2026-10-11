#include "pacer.h"
#include "barrier.h"
#include "blend/frame.h"
#include "clock.h"
#include "config.h"
#include "crashguard.h"
#include "gamestate.h"
#include "log.h"
#include "patch.h"

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace
{
constexpr uintptr_t kFramePacer = 0xAC82B0;
// sub esp,10h / test byte ptr [...], 1 - as it looks once the exe has
// decrypted its code.
constexpr uint8_t kPacerPrologue[7] = { 0x83, 0xEC, 0x10, 0xF6, 0x05, 0xD0, 0x26 };

// dword_118371C is VsyncInterval as the renderer consumes it (copied into the
// device parameters at 0x10E6D68+0x4C and passed to Present as SyncInterval).
constexpr uintptr_t kVsyncIntervalGlobal = 0x118371C;
constexpr uintptr_t kPresentSyncSlot = 0x10E6D68 + 0x4C;
constexpr uintptr_t kSwapChainSlot = 0x11ACDE4;   // non-null once the device exists

// The engine's frame clock (the pacer's `this`).
struct FrameClock
{
    int32_t mode;        // +0x00
    int32_t f04;         // +0x04
    int64_t lastNow;     // +0x08  last time seen, 100 ns
    int64_t accum;       // +0x10
    int64_t budget;      // +0x18
    int64_t period;      // +0x20  tick period, 100 ns (166670 at 60 Hz)
    int32_t lastTicks;   // +0x28
    int32_t f2C;         // +0x2C
    int32_t f30;         // +0x30
    uint8_t noWait;      // +0x34  paused / special state: return minTicks
    uint8_t onTime;      // +0x35
    uint8_t f36;         // +0x36
};

int64_t s_lastTickTime = 0;   // boundary of the last issued tick, 100 ns
// The clock as the last pacer call saw it, for TickDueBeforeNextFrame.
bool    s_ticking = false;    // normal mode: ticks issued by wall-clock time
int64_t s_period = 0;
int     s_minTicks = 0;
int64_t s_nextFrame = 0;      // frame cap deadline

volatile LONG s_frames = 0;
volatile LONG s_ticks = 0;
volatile LONG s_lastTicks = 0;

// Every exit of the pacer: let the render workers drain the frame just
// presented, then put back every value the interpolation wrote for it.
// This runs on paused frames too - they still reset the command arena.
void EndOfFrame()
{
    EndFrameBackpressure();
    WaitRenderDrain();
    frame::Restore();
}

// Start and loading screens render in a fraction of a millisecond and would
// otherwise run at thousands of frames per second.
constexpr int kLoadingFps = 60;

LoadingScreen s_loadingScreen = LoadingScreen::None;   // set once per frame by HookFramePacer

// The frame cap for this frame: TargetFPS, or 60 on a start or loading screen.
int FrameCap()
{
    if (s_loadingScreen == LoadingScreen::None)
        return g_config.targetFps;
    return g_config.targetFps > 0 && g_config.targetFps < kLoadingFps ? g_config.targetFps
                                                                      : kLoadingFps;
}

void NoteLoadingScreen(LoadingScreen screen)
{
    static int64_t s_since = 0;
    const bool was = s_loadingScreen != LoadingScreen::None;
    const bool now = screen != LoadingScreen::None;
    s_loadingScreen = screen;
    if (was == now)
        return;    // includes a change of kind mid-load (an event load briefly unloads the world)
    const int64_t t = Now100ns();
    if (now)
        LOG_INFO("%s loading screen: capped at %d fps", LoadingScreenName(screen), kLoadingFps);
    else
        LOG_INFO("loading screen closed after %.1f s", (t - s_since) / 1.0e7);
    s_since = t;
}

// Frames that take far longer than any normal one are logged, so a reported
// freeze can be matched against what the mod and the game were doing. Loading
// screens and frames while the game is in the background are left out.
constexpr int64_t kLongFrame100 = 250 * 10000;   // 250 ms
constexpr int kMaxLongFrameReports = 32;

void NoteFrameTime()
{
    static int64_t s_prev = 0;
    static int s_reports = 0;
    const int64_t now = Now100ns();
    const int64_t dt = s_prev ? now - s_prev : 0;
    s_prev = now;
    if (dt < kLongFrame100 || s_loadingScreen != LoadingScreen::None ||
        s_reports >= kMaxLongFrameReports)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId())
        return;
    ++s_reports;
    LOG_WARN("long frame: %lld ms (crash guard catches so far: %ld)", dt / 10000,
             EmulatedFaultCount());
}

int Pace(FrameClock* self, int minTicks, int maxTicks, int* outTicks)
{
    const int64_t now = Now100ns();

    s_ticking = !self->noWait && minTicks > 0 && self->period > 0 && s_lastTickTime != 0;
    s_period = self->period;
    s_minTicks = minTicks;

    if (self->noWait || minTicks <= 0)
    {
        s_lastTickTime = 0;          // reseed on resume
        *outTicks = minTicks;
        EndOfFrame();
        return minTicks;
    }

    // A freshly (re)initialised clock: seed.
    if (s_lastTickTime == 0 || self->lastNow == 0)
    {
        s_lastTickTime = now;
        self->lastNow = now;
        *outTicks = minTicks;
        EndOfFrame();
        return minTicks;
    }

    const int64_t period = self->period;
    if (period <= 0)
    {
        *outTicks = minTicks;
        EndOfFrame();
        return minTicks;
    }

    const int cap = FrameCap();
    if (cap > 0)
    {
        const int64_t framePeriod = 10000000i64 / cap;
        if (s_nextFrame == 0)
            s_nextFrame = now + framePeriod;
        while (Now100ns() < s_nextFrame)
        {
            if (s_nextFrame - Now100ns() > 15000)   // more than 1.5 ms left: sleep
                Sleep(1);
        }
        s_nextFrame += framePeriod;
        if (s_nextFrame < Now100ns())              // fell behind: resync
            s_nextFrame = Now100ns() + framePeriod;
    }
    else
    {
        s_nextFrame = 0;
    }

    // Ticks owed by wall-clock time. minTicks doubles as the speed multiplier
    // in fast-forward states (vanilla returns minTicks per 60 Hz frame there).
    int64_t due = (Now100ns() - s_lastTickTime) * minTicks / period;
    if (due <= 0)
    {
        *outTicks = 0;               // render-only frame
        EndOfFrame();
        return 0;
    }
    if (due > maxTicks)
    {
        due = maxTicks;
        s_lastTickTime = Now100ns(); // resync instead of spiralling
    }
    else
    {
        s_lastTickTime += due * period / minTicks;
    }
    self->lastNow = Now100ns();
    *outTicks = static_cast<int>(due);
    EndOfFrame();
    return static_cast<int>(due);
}

int __fastcall HookFramePacer(FrameClock* self, int /*edx*/, int minTicks, int maxTicks,
                              int* outTicks)
{
    if (g_config.capLoading)
        NoteLoadingScreen(CurrentLoadingScreen());
    NoteFrameTime();
    const int ticks = Pace(self, minTicks, maxTicks, outTicks);
    InterlockedExchange(&s_lastTicks, ticks);
    InterlockedExchangeAdd(&s_ticks, ticks);
    InterlockedIncrement(&s_frames);

    // Publish the tick grid so the next frame can compute how far it is into
    // the current tick: (now - s_lastTickTime) / period.
    const int64_t period = self->period;
    const bool valid = s_lastTickTime != 0 && period > 0;
    frame::SetPhase(s_lastTickTime, valid && minTicks > 0 ? period / minTicks : period, ticks,
                    valid);
    return ticks;
}

DWORD WINAPI ApplyVsyncOverride(LPVOID)
{
    for (int i = 0; i < 3000; ++i)
    {
        if (*reinterpret_cast<volatile uintptr_t*>(kSwapChainSlot) != 0)
        {
            const int32_t v = g_config.vsyncInterval;
            patch::Write(kVsyncIntervalGlobal, &v, sizeof(v));
            patch::Write(kPresentSyncSlot, &v, sizeof(v));
            LOG_INFO("vsync interval set to %d", v);
            return 0;
        }
        Sleep(50);
    }
    LOG_WARN("vsync override: the device did not appear within 150 s");
    return 0;
}
} // namespace

bool PacerSiteOk()
{
    return patch::Matches(kFramePacer, kPacerPrologue, sizeof(kPacerPrologue));
}

void InstallPacer()
{
    patch::Jump(kFramePacer, reinterpret_cast<const void*>(&HookFramePacer));
    timeBeginPeriod(1);              // 1 ms Sleep granularity for the frame caps
    if (g_config.vsyncInterval >= 0)
    {
        HANDLE h = CreateThread(nullptr, 0, &ApplyVsyncOverride, nullptr, 0, nullptr);
        if (h)
            CloseHandle(h);
    }
}

long FrameCount() { return InterlockedCompareExchange(&s_frames, 0, 0); }
long TickCount()  { return InterlockedCompareExchange(&s_ticks, 0, 0); }
int  LastTicks()  { return InterlockedCompareExchange(&s_lastTicks, 0, 0); }

bool TickDueBeforeNextFrame()
{
    if (!s_ticking)
        return true;   // the game issues no ticks by time right now: nothing to wait for
    return (Now100ns() - s_lastTickTime) * s_minTicks >= s_period;
}
