#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// Per-frame interpolation driver.
//
// The simulation stores its state once per 60 Hz tick and the renderer draws
// whatever is stored, so above 60 fps the extra frames show the same world.
// Each fix in this mod is the same operation: keep the last two tick states of
// a store the renderer reads, write the blend between them at the frame's
// position inside the current tick, and put the true value back before the
// simulation can see it. This module owns the timing of that operation; the
// stores live in the other files of this folder.
//
// The frame, as the game's frame function runs it:
//
//   0x476FA5  call sub_4766A0   subsystem pass; sim ticks run inside it
//   0x476FAC  call sub_46F730   render phase
//               0x46F987  call sub_8C7FE0   camera / render dispatch
//               0x46FA1D  call sub_8CFFA0   frame render controller: resets the
//                                           render descriptors and dispatches the
//                                           worker build jobs
//   ... Present, then the frame pacer (pacer.cpp)
//
// Blends are applied before the subsystem pass on frames with no tick due,
// so the things the engine composes from other objects in there (the driver
// model, world-anchored readouts) are built from blended parents. On tick
// frames the tick must see truth, so the blends are applied at the start of
// the render phase instead. They are taken back out right after the render
// controller returns (objects, views) and after Present (everything else).
namespace frame
{
bool SitesOk();
void Install();

// From the pacer, once per frame: wall time (100 ns) of the last issued tick
// boundary, the effective tick period, and the ticks issued (>1 = a catch-up
// batch, shown unblended).
void SetPhase(int64_t phaseBase, int64_t periodEff, int lastDue, bool valid);

// End of frame (pacer, after Present and the worker drain).
void Restore();

uint32_t  Index();        // interpolated frames so far
float     Alpha();        // blend factor of the current frame, 0..1
float     RawAlpha();     // the tick phase itself, even on catch-up frames
bool      InWindow();     // blends are live in memory right now
DWORD     GameThread();
uintptr_t GameObject();   // the game's main object (`this` of the frame calls)
} // namespace frame
