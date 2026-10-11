#pragma once

// Entity transforms: cars, traffic, police, props, street lamps, loose parts.
//
// Every entity class answers the engine-wide "give me your 4x4" call through
// vtable slot 0. The trivial getters (`lea eax,[ecx+N]; ret`) and the three
// classes whose matrix lives in a shared array are hooked, which registers
// each object the renderer asks about in a map. Once per frame the map is
// walked: each object's transform is latched when a tick changes it, and the
// blend of its last two tick values is written in place for the frame.
namespace objects
{
// Returns the number of getters hooked (11 expected).
int InstallGetterHooks();

// Frame start: registrations queued by worker threads, then every tracked
// object is blended in place.
void BeginFrame(float alpha);
// Drops objects nothing has asked about for a while.
void Prune();
// The driver-model composition (character.cpp) rewrites the small props from
// tick state; this puts their blend back.
void Reapply();
// Puts the true transforms back.
void Restore();

// The last two tick transforms of a tracked object.
bool TickPair(const void* obj, const float** prev, const float** cur);
} // namespace objects
