#include "parts.h"
#include "frame.h"
#include "xform.h"
#include "../patch.h"

#include <cstring>

// Car-attached effects.
//
// The per-frame VFX job (sub_4A1470, inside the blend window) calls
// sub_49BC60(requests, dt, x) at 0x4A20A0. The requests are 320 records of
// 240 bytes the game writes per tick (effectsMgr + 42288). For each active
// request (+232 bit 0) of type 1/2/3/5 it copies the 64-byte transform at +0
// into the effect instance; type 4 (wheel contact) reads it the same way.
// Type 6 (light cones) goes through the entity getter, which is already
// blended - why lights were smooth and flames stepped.
//
// Around the call each request's transform is replaced by the blend of its
// last two tick values, and restored as soon as the call returns.
namespace parts
{
namespace
{
constexpr uintptr_t kSite   = 0x4A20A0;
constexpr uintptr_t kTarget = 0x49BC60;
constexpr int kRequests = 320, kStride = 240;
constexpr int kTypeMask = 0x3E;              // types 1..5

struct Slot
{
    float    cur[16];
    float    prev[16];
    uint32_t def;
    uint8_t  type;
    bool     have;
};
Slot  g_slot[kRequests];
float g_save[kRequests][16];

using Fn = unsigned(__fastcall*)(void*, void*, int, float, float);

unsigned __fastcall EffectRequestsHook(void* self, void* edx, int reqs, float a3, float a4)
{
    const bool window = frame::InWindow();
    const float a = frame::Alpha();
    bool wrote[kRequests];

    uint8_t* base = reinterpret_cast<uint8_t*>(reqs);
    for (int i = 0; i < kRequests && base; ++i)
    {
        wrote[i] = false;
        uint8_t* r = base + i * kStride;
        Slot& s = g_slot[i];
        const uint8_t flags = r[232];
        const uint8_t type = r[233];
        if (!(flags & 1) || type > 7)
        {
            s.have = false;
            continue;
        }
        float* m = reinterpret_cast<float*>(r);
        const uint32_t def = *reinterpret_cast<const uint32_t*>(r + 224);
        if (!blend::Finite16(m))
        {
            s.have = false;
            continue;
        }
        if (!s.have || s.def != def || s.type != type)
        {
            memcpy(s.cur, m, sizeof(s.cur));
            memcpy(s.prev, m, sizeof(s.prev));
            s.def = def;
            s.type = type;
            s.have = true;
        }
        else if (memcmp(s.cur, m, sizeof(s.cur)) != 0)
        {
            memcpy(s.prev, s.cur, sizeof(s.prev));
            memcpy(s.cur, m, sizeof(s.cur));
        }
        if (!(kTypeMask & (1 << type)) || !window ||
            memcmp(s.prev, s.cur, sizeof(s.cur)) == 0)
            continue;
        const float jx = s.cur[12] - s.prev[12], jy = s.cur[13] - s.prev[13],
                    jz = s.cur[14] - s.prev[14];
        if (jx * jx + jy * jy + jz * jz > 400.0f)
            continue;                                // a reset, not motion
        float out[16];
        if (type == 4)
            for (int k = 0; k < 16; ++k)
                out[k] = s.prev[k] + (s.cur[k] - s.prev[k]) * a;
        else
            blend::LerpTransform(out, s.prev, s.cur, a);
        memcpy(g_save[i], m, sizeof(g_save[i]));
        memcpy(m, out, sizeof(out));
        wrote[i] = true;
    }

    const unsigned ret = reinterpret_cast<Fn>(kTarget)(self, edx, reqs, a3, a4);

    for (int i = 0; i < kRequests && base; ++i)
        if (wrote[i])
            memcpy(base + i * kStride, g_save[i], sizeof(g_save[i]));
    return ret;
}
} // namespace

bool InstallEffectRequests()
{
    return patch::RedirectCall(kSite, kTarget, reinterpret_cast<const void*>(&EffectRequestsHook));
}
} // namespace parts
