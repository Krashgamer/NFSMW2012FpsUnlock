#pragma once

// World streaming on the game's original schedule.
//
// The frame runs: subsystem pass (the 60 Hz simulation ticks happen inside
// it), render, then one step of the streaming manager (sub_47E1E0), which
// loads bundles and frees the memory of unloaded ones. The world system
// removes the draw instances of unloaded sections in its tick update; the
// render pass culls the scene every frame.
//
// Without the mod every frame has a tick, so memory freed by a streaming step
// is always followed by a tick that removes its instances before anything is
// drawn again. Uncapped, most frames have no tick: the frames after a step
// still cull and draw those instances, reading bundle memory that has been
// freed and refilled by the loader. That is the freeze (thousands of caught
// faults) and the crash in streaming-heavy places such as tunnels.
//
// So the step only runs on frames where a tick is already due: the pacer then
// issues at least one tick for the next frame, and the original order -
// stream, tick, draw - holds again, with no waiting. That is at most one step
// per tick; a tick that falls due after the step's place in the frame has
// none, so streaming runs at roughly 25-60 steps a second (about 28 at
// 160 fps), the range the game was built for (30 fps consoles, 60 fps PC).
// In states where the game issues no ticks (paused, special clock modes) and
// on start and loading screens the step runs every frame, as it does without
// the mod.
bool InstallStreamSync();
