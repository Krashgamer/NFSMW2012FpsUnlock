#include "parts.h"
#include "frame.h"
#include "latch.h"
#include "saferead.h"
#include "../patch.h"

// Pieces of broken multi-part props.
//
// A prop that breaks into pieces (lamp post, sign, speed camera) is drawn as
// a skinned mesh: the prop entity's world matrix (blended by objects.cpp) plus
// one bone per piece in gBoneMatrices4WeightsReduced. sub_535510(entity, ctx)
// builds that palette on the render thread, for each piece
// (entity+0x10C count, entity+0x108 pointer array):
//
//   bone = piece+0x134 (byte)
//   m    = bind(bone) x piece matrix (piece+0xF0, written once per tick)
//   sub_53DB60(bone, &m, palette)          <- the single call site 0x5356FA
//
// The call is redirected to pass a blended copy of m, keyed by the piece.
namespace parts
{
namespace
{
constexpr uintptr_t kPaletteFn   = 0x53DB60;
constexpr uintptr_t kPaletteCall = 0x5356FA;

blend::TickLatch<512> g_latch;

using PaletteFn = int(__cdecl*)(int, const float*, void*);
} // namespace

extern "C" int __cdecl PropPieceHook(uintptr_t piece, uintptr_t /*entity*/, int bone,
                                     const float* m, void* palette)
{
    __declspec(align(16)) float out[16];
    const float* use = m;
    float mm[16];
    if (piece && !(piece & 3) &&
        blend::SafeRead(reinterpret_cast<uintptr_t>(m), mm, sizeof(mm)) && blend::Finite16(mm))
    {
        use = g_latch.Blend(piece, mm, out, frame::Alpha());
        if (use == mm)
            use = m;
    }
    return reinterpret_cast<PaletteFn>(kPaletteFn)(bone, use, palette);
}

// At the call site: [esp+4] bone, [esp+8] m, [esp+12] palette; esi = piece,
// edi = entity. Re-push the arguments under (piece, entity); cdecl.
extern "C" __declspec(naked) void PropPieceThunk()
{
    __asm push dword ptr [esp + 12]      // palette
    __asm push dword ptr [esp + 12]      // m
    __asm push dword ptr [esp + 12]      // bone
    __asm push edi
    __asm push esi
    __asm call PropPieceHook
    __asm add esp, 20
    __asm ret
}

bool InstallPropPieces()
{
    return patch::RedirectCall(kPaletteCall, kPaletteFn,
                               reinterpret_cast<const void*>(&PropPieceThunk));
}
} // namespace parts
