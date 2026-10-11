#pragma once
#include <cstdint>

// Transform blending. Game transforms are row-major 4x4 floats: three basis
// rows (xyz, unused w) and the translation in floats 12..14.
namespace blend
{
// All 12 transform floats finite and inside +-1e5 (the w column is ignored:
// the player car keeps an unrelated, ever-growing value in [15]).
bool Plausible(const float* t);

// All 16 floats finite and inside +-1e6.
bool Finite16(const float* m);

// Squared distance between the translations of two transforms.
float PosDist2(const float* a, const float* b);

// Rotation-aware blend: per-row scale and translation lerp, rotation slerp.
// A wheel turns ~88 degrees in one 60 Hz tick at 100 km/h; a component-wise
// blend of two bases that far apart shrinks them and makes the angular rate
// uneven. Anything that is not a scaled right-handed orthonormal basis falls
// back to the component blend.
void LerpTransform(float* out, const float* a, const float* b, float t);

// Is this dword plausibly a geometry float, rather than a pointer, integer,
// flag or handle? Finite, and either exactly zero or with a magnitude in
// [1e-20, 1e7]: heap pointers land near 1e-31, small integers and flags are
// denormals. Used to blend whole engine structs field by field.
bool Lerpable(uint32_t bits);

// Blends `n` dwords of two struct snapshots: fields that are equal or not
// Lerpable are copied from `cur`. Returns the number of fields blended.
long LerpFields(uint32_t* out, const uint32_t* prev, const uint32_t* cur, size_t n, float t);
} // namespace blend
