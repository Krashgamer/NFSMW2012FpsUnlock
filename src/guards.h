#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Defensive patches for the game's own render-state handling under uncapped
// frame rates. See guards.cpp.

// Bounds checks on the descriptor slot index (4 sites).
int InstallSlotBoundsGuards();

// Opcode bound on the command-stream replay dispatcher, plus a sanitiser on
// every count+array bind handler. Returns the number of sites patched.
int InstallReplayGuards();

// Set by the crash guard the first time it emulates a fault; the bind
// sanitiser stays a single compare until then.
extern volatile LONG g_sanitizeArmed;
