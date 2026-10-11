#pragma once

// Render worker barrier.
//
// The game's renderer hands each frame's command building to job-system
// worker threads and double-buffers the command arena. At the vanilla 60 fps
// cap the game thread always waited long enough for those workers to finish
// before it reset and reused that state for the next frame. Uncapped, the game
// thread can get there first, and the workers then read render objects that
// are being torn down and rebuilt underneath them, and the game crashes.
//
// The barrier restores the ordering without restoring the cap: at the points
// where the game thread is about to reset or reuse render state, it waits
// until no job is running and none is queued.

// The job invoker and the worker loop hold the expected code.
bool BarrierSitesOk();
// Hooks both. The frame rate may only be unlocked when this succeeds.
bool InstallBarrierHooks();

// Waits until the render job pipeline is idle (bounded).
void WaitRenderDrain();

// End of frame: hand the timeslice to the workers before the next frame.
void EndFrameBackpressure();
