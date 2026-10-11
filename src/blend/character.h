#pragma once

// The driver model (and every other character in a car).
//
// Characters are composed by sub_53AD40 (12 slots at gameObj+0x2A8D00): for
// each slot sub_538270 places the character in the world (sub_534990, from
// entity attachment transforms), animates it, and emits its 4x4s into the
// render constant stream (sub_481EA0), all inside the tick. The placement is
// never stored anywhere a blend could reach, so the composition itself is run
// again every frame, after the blends it reads have been applied.
namespace character
{
// Blends the entity attachment transforms the composition places the
// character from. Must run before Compose().
void ApplyAnchors(float alpha);
void RestoreAnchors();

// Runs the character composition (sub_53AD40) once more for this frame.
void Compose();

// Blends the pose output the AnimationJob wrote for each slot.
void ApplyPose(float alpha);
void RestorePose();
} // namespace character
