#pragma once

// Crash guard for the render-object race that uncapped frame rates expose.
// The render bucket work runs on job-system workers, and with the 60 Hz
// pacing gone the game thread and the workers can observe render objects
// mid-construction. On an access violation in game code the guard decodes the
// faulting instruction and emulates it as if the memory operand held 0, which
// turns a crash into a one-frame glitch. Everything it cannot handle is
// written to the log as a crash report and passed on.

void InstallCrashGuard();

// Faults emulated so far.
long EmulatedFaultCount();
