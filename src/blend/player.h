#pragma once

// Which entity is the player's car.
//
// The game keeps a proximity registry of entities (gameObj+0x4A00A0), merged
// once per tick by sub_A9A370 from a queue that names every live slot; the
// slot type 2 is the player. Its position, compared against each car the car
// getter returns, identifies the player car object.
namespace player
{
bool InstallRegistryHook();

// From the car getter (objects.cpp).
void NoteCarTransform(const void* obj);

// Latches the car object for this frame (frame start).
void BeginFrame();
const void* CarObject();
} // namespace player
