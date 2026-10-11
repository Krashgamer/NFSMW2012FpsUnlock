#pragma once

// Matrices the game copies straight into a draw once per tick, blended at
// the copy. None of these write game memory: the draw is handed a blended
// copy, or the request is restored right after the call that reads it.
namespace parts
{
// Car-attached effects: nitrous flame, popped-rim sparks, wheel effects.
bool InstallEffectRequests();
// Wheels of cars in physics mode (traffic that has been hit, parked props).
bool InstallPhysicsWheels();
// Pieces of broken multi-part props (lamp posts, signs, speed cameras).
bool InstallPropPieces();
} // namespace parts
