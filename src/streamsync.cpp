#include "streamsync.h"
#include "clock.h"
#include "gamestate.h"
#include "pacer.h"
#include "patch.h"

#include <cstdint>

namespace
{
constexpr uintptr_t kStreamCall   = 0x476FD6;   // call sub_47E1E0 (ecx = game object + 0x3400)
constexpr uintptr_t kStreamUpdate = 0x47E1E0;

volatile LONG s_steps = 0;
volatile LONG s_stepsTickDue = 0;
volatile LONG s_longestGapMs = 0;
int64_t s_lastStep = 0;

extern "C" bool __cdecl StreamSyncShouldStep()
{
    const TickForecast tick = ForecastTick();   // every frame: it times the frame's tail
    // Start and loading screens step every frame, as without the mod. The
    // faded-out world (car switch, map) does not count: its instances still
    // exist, so the step keeps the tick rule there. Capped at 60 that is one
    // step per frame anyway.
    const LoadingScreen screen = CurrentLoadingScreen();
    const bool loading = screen == LoadingScreen::World || screen == LoadingScreen::Event;
    if (!loading && tick == TickForecast::None)
        return false;

    InterlockedIncrement(&s_steps);
    if (loading || tick == TickForecast::Due)
        InterlockedIncrement(&s_stepsTickDue);
    const int64_t now = Now100ns();
    if (s_lastStep != 0 && (now - s_lastStep) / 10000 > s_longestGapMs)
        InterlockedExchange(&s_longestGapMs, static_cast<LONG>((now - s_lastStep) / 10000));
    s_lastStep = now;
    return true;
}
} // namespace

long StreamStepCount()        { return InterlockedCompareExchange(&s_steps, 0, 0); }
long StreamStepTickDueCount() { return InterlockedCompareExchange(&s_stepsTickDue, 0, 0); }
long TakeLongestStreamGapMs() { return InterlockedExchange(&s_longestGapMs, 0); }

bool InstallStreamSync()
{
    if (patch::CallTarget(kStreamCall) != kStreamUpdate)
        return false;
    uint8_t* mem = patch::AllocCode(32);
    if (!mem)
        return false;
    // push ecx / call decide / pop ecx / test al,al / jz skip /
    // jmp sub_47E1E0 (returns to the call site) / skip: ret
    patch::Stub s{ mem };
    s.B(0x51);
    s.Call(reinterpret_cast<const void*>(&StreamSyncShouldStep));
    s.B(0x59);
    s.B(0x84); s.B(0xC0);
    s.B(0x74); s.B(0x05);
    s.Jmp(kStreamUpdate);
    s.B(0xC3);
    patch::Branch(kStreamCall, 0xE8, mem);
    return true;
}
