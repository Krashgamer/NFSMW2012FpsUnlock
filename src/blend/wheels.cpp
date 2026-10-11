#include "parts.h"
#include "frame.h"
#include "latch.h"
#include "saferead.h"
#include "../patch.h"

// Wheels of cars in physics mode.
//
// The car renderer fills a per-car draw record each pass (sub_583290): the
// body comes from the car getter (blended), and each child part is copied in
// by sub_8E14C0(record; slot, mesh, src). For a car the physics simulation
// owns - traffic that has been hit, parked cars - sub_5F7CF0 builds each
// wheel from the physics wheel transforms and hands it over at 0x5F92C5.
// That matrix is world space and written once per tick, so those wheels
// stepped at 60 Hz under a smooth body.
//
// The call is redirected to pass a blended copy instead.
namespace parts
{
namespace
{
constexpr uintptr_t kCopyFn   = 0x8E14C0;
constexpr uintptr_t kCopyCall = 0x5F92C5;

blend::TickLatch<1024> g_latch;

using CopyFn = void*(__fastcall*)(void*, void*, int, int, const float*);
} // namespace

// The caller keeps its `this` (the vehicle render object) in esi across the
// call; the thunk passes it in edx, which the __thiscall callee ignores.
// Key: vehicle + wheel slot.
void* __fastcall PhysicsWheelHook(void* record, void* vehicle, int slot, int mesh, const float* src)
{
    __declspec(align(16)) float out[16];
    const float* use = src;
    const uintptr_t self = reinterpret_cast<uintptr_t>(vehicle);
    float m[16];
    if (self && !(self & 3) &&
        blend::SafeRead(reinterpret_cast<uintptr_t>(src), m, sizeof(m)) && blend::Finite16(m))
    {
        const uintptr_t key = self + (static_cast<uintptr_t>(slot) & 15) * 4 + 2;
        use = g_latch.Blend(key, m, out, frame::Alpha());
        if (use == m)
            use = src;
    }
    return reinterpret_cast<CopyFn>(kCopyFn)(record, vehicle, slot, mesh, use);
}

extern "C" __declspec(naked) void PhysicsWheelThunk()
{
    __asm mov edx, esi
    __asm jmp PhysicsWheelHook
}

bool InstallPhysicsWheels()
{
    return patch::RedirectCall(kCopyCall, kCopyFn, reinterpret_cast<const void*>(&PhysicsWheelThunk));
}
} // namespace parts
