#pragma once
#include <cstdint>

// The frame pacer replacement - the actual unlock.
//
// sub_AC82B0 is called once per rendered frame (from sub_46FB60, after
// Present) and returns how many 60 Hz simulation ticks the next frame runs.
// Vanilla busy-waits until the next tick is due, which is the 60 fps cap. The
// replacement never waits: it returns the ticks owed by wall-clock time, 0 on
// render-only frames, so the simulation keeps its fixed 60 Hz while rendering
// runs as fast as it can.
bool PacerSiteOk();
void InstallPacer();

// Frames presented and simulation ticks issued since startup.
long FrameCount();
long TickCount();
// Ticks issued for the current frame (0 = render-only frame).
int LastTicks();

// Whether the next pacer call issues at least one tick, asked at the streaming
// step's place in the frame (streamsync.cpp). Game thread, once every frame:
// the call also times the rest of the frame.
enum class TickForecast
{
    None,       // the next frame is a render-only frame
    Due,        // a tick is already due (or the clock is in a mode that issues
                // none by time)
    Promised,   // not due yet, but it will be by the time the pacer runs: the
                // rest of the frame (Present, vsync, a frame limiter) or the
                // frame cap takes at least that long. The pacer keeps the
                // promise - should the frame come round sooner, it issues
                // the tick a moment early. It never waits for it.
};
TickForecast ForecastTick();

// How often, and by how much in total, the pacer issued a promised tick
// early.
long EarlyTickCount();
long EarlyTickMicros();
