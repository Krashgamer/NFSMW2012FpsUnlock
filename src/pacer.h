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

// True when the next pacer call is certain to issue at least one tick (or the
// clock is in a mode that issues none by time). Game thread.
bool TickDueBeforeNextFrame();
