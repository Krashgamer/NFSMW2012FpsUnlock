#include "character.h"
#include "frame.h"
#include "xform.h"
#include "../log.h"

#include <cmath>
#include <cstring>

namespace character
{
namespace
{
// Per-slot layout inside the character manager at gameObj+0x2A8D00:
//   mgr + 3008*i            instance state
//   mgr + 36288 + 1568*i    pose output, handed to sub_481EA0
constexpr uint32_t kCharMgrOff = 0x2A8D00;
constexpr size_t   kSlots      = 12;
constexpr size_t   kStride     = 3008;
constexpr uint32_t kPoseOff    = 36288;
constexpr size_t   kPoseBytes  = 1568;
constexpr size_t   kPoseDw     = kPoseBytes / 4;

constexpr uintptr_t kComposeFn = 0x53AD40;   // sub_53AD40
using ComposeFn = void(__thiscall*)(void*);
bool g_composeOn = true;

uint8_t* Manager()
{
    const uintptr_t go = frame::GameObject();
    return go ? reinterpret_cast<uint8_t*>(go) + kCharMgrOff : nullptr;
}

// The activity test sub_53AD40 makes before it dispatches a slot.
bool SlotActive(const uint8_t* mgr, size_t i)
{
    const uint8_t* b = mgr + kStride * i;
    __try
    {
        if (*(b + 3184) == 0)
            return false;
        if (*reinterpret_cast<const uint32_t*>(b + 3188) == 0xFFFFFFFFu)
            return false;
        if (*reinterpret_cast<const uint32_t*>(b + 3192) == 0xFFFFFFFFu)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pose output
// ---------------------------------------------------------------------------
struct PoseState
{
    uint32_t prev[kPoseDw];
    uint32_t cur[kPoseDw];
    uint32_t out[kPoseDw];
    bool have;
    bool wrote;
};
PoseState g_pose[kSlots];

// ---------------------------------------------------------------------------
// Attachment anchors
// ---------------------------------------------------------------------------
// sub_534990 reads the character's world placement through two handles of its
// entity (at 0x534B4D / 0x534F3D):
//
//   ent   = *(*(0x10EDAE4) + 4) + 7232*id + 12368          (sub_5734A0)
//   alive = *(u8*)(ent + 25)                                (sub_573390)
//   a = *(*(ent+208) + 40), b = *(ent+212)
//   X = *(a+b+44), Y = *(a+b+48), C = *(X+40)
//   h1 = *(C+Y+48)  -> 2 (quat, pos) pairs read
//   h2 = *(C+Y+44)  -> 1 (quat, pos) pair read
//
// A handle resolves through the entity's own table (sub_57C4B0 + sub_42B870)
// to an aligned float4 array: quaternion, position, quaternion, position...
constexpr uintptr_t kEntMgrPtr = 0x10EDAE4;
constexpr size_t    kBlocks    = 2;
constexpr size_t    kMaxVecs   = 4;
constexpr size_t    kMaxDw     = kMaxVecs * 4;

struct AnchorBlock
{
    float*   at;
    uint32_t vecs;
    uint32_t prev[kMaxDw];
    uint32_t cur[kMaxDw];
    uint32_t out[kMaxDw];
    bool have;
    bool wrote;        // written this frame, awaiting restore
    bool outValid;     // out still describes what we last wrote
};
AnchorBlock g_anchor[kSlots][kBlocks];

// sub_57C4B0 -> sub_42B870, reimplemented: pure loads and a linear scan.
float* HandleTransform(uint32_t ent, uint32_t handle)
{
    uint32_t tbl = *reinterpret_cast<const uint32_t*>(ent + 0x168);
    if (!tbl)
    {
        const uint32_t e = *reinterpret_cast<const uint32_t*>(ent + 0x184);
        if (!e)
            return nullptr;
        tbl = *reinterpret_cast<const uint32_t*>(e + 0x28) +
              *reinterpret_cast<const uint32_t*>(ent + 0x188) + 0x1C;
    }
    const uint32_t n    = *reinterpret_cast<const uint32_t*>(tbl + 0);
    const uint32_t keys = *reinterpret_cast<const uint32_t*>(tbl + 4);
    const uint32_t vals = *reinterpret_cast<const uint32_t*>(tbl + 8);
    if (!vals)
        return nullptr;
    uint32_t rec = vals;
    if (n)
    {
        if (!keys || n > 65536u)
            return nullptr;
        uint32_t i = 0;
        for (; i < n; ++i)
            if (*reinterpret_cast<const uint32_t*>(keys + 4u * i) == handle)
                break;
        if (i < n)
            rec = vals + 12u * i;      // a miss falls back to index 0, as the original does
    }
    const uint32_t xf = *reinterpret_cast<const uint32_t*>(rec + 4);
    if (!xf || (xf & 15u) != 0)
        return nullptr;                // read with movaps: must be aligned
    return reinterpret_cast<float*>(xf);
}

bool ResolveAnchors(uint32_t entityId, float** xf1, float** xf2)
{
    *xf1 = nullptr;
    *xf2 = nullptr;
    __try
    {
        const uint32_t mgrp = *reinterpret_cast<const uint32_t*>(kEntMgrPtr);
        if (!mgrp)
            return false;
        const uint32_t inner = *reinterpret_cast<const uint32_t*>(mgrp + 4);
        if (!inner)
            return false;
        const uint32_t ent = inner + 7232u * entityId + 12368u;
        if (*reinterpret_cast<const uint8_t*>(ent + 25) == 0)
            return false;
        const uint32_t p208 = *reinterpret_cast<const uint32_t*>(ent + 208);
        if (!p208)
            return false;
        const uint32_t a = *reinterpret_cast<const uint32_t*>(p208 + 40);
        const uint32_t b = *reinterpret_cast<const uint32_t*>(ent + 212);
        if (!a)
            return false;
        const uint32_t X = *reinterpret_cast<const uint32_t*>(a + b + 44);
        const uint32_t Y = *reinterpret_cast<const uint32_t*>(a + b + 48);
        if (!X)
            return false;
        const uint32_t C = *reinterpret_cast<const uint32_t*>(X + 40);
        if (!C)
            return false;
        const uint32_t h1 = *reinterpret_cast<const uint32_t*>(C + Y + 48);
        const uint32_t h2 = *reinterpret_cast<const uint32_t*>(C + Y + 44);
        if (h1)
            *xf1 = HandleTransform(ent, h1);
        if (h2)
            *xf2 = HandleTransform(ent, h2);
        return *xf1 != nullptr || *xf2 != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Even float4s are quaternions (normalised lerp), odd ones world positions.
void BlendBlock(AnchorBlock& d, float a)
{
    const size_t nd = d.vecs * 4u;
    const size_t nb = nd * sizeof(uint32_t);
    uint32_t v[kMaxDw];
    __try
    {
        memcpy(v, d.at, nb);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        d.have = false;
        return;
    }
    if (!d.have)
    {
        memcpy(d.cur, v, nb);
        memcpy(d.prev, v, nb);
        d.have = true;
        d.outValid = false;
        return;
    }
    const bool ours = d.outValid && memcmp(v, d.out, nb) == 0;
    if (!ours && memcmp(v, d.cur, nb) != 0)
    {
        memcpy(d.prev, d.cur, nb);
        memcpy(d.cur, v, nb);
    }
    if (memcmp(d.prev, d.cur, nb) == 0)
        return;

    long blended = 0;
    for (uint32_t vi = 0; vi < d.vecs; ++vi)
    {
        uint32_t* o = &d.out[vi * 4];
        const uint32_t* bp = &d.prev[vi * 4];
        const uint32_t* bc = &d.cur[vi * 4];
        float p[4], c[4], r[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k)
        {
            if (!blend::Lerpable(bp[k]) || !blend::Lerpable(bc[k]))
            {
                ok = false;
                break;
            }
            memcpy(&p[k], &bp[k], sizeof(float));
            memcpy(&c[k], &bc[k], sizeof(float));
        }
        if (!ok)
        {
            memcpy(o, bc, 16);
            continue;
        }
        if ((vi & 1u) == 0u)
        {
            // q and -q are the same rotation; a flip across a tick would
            // sweep the long way round. One tick never rotates that far.
            const float dot = p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] * c[3];
            if (dot < 0.9f)
            {
                memcpy(o, bc, 16);
                continue;
            }
            float n2 = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                r[k] = p[k] + (c[k] - p[k]) * a;
                n2 += r[k] * r[k];
            }
            // sub_534990 assumes a unit quaternion; unnormalised it would
            // scale the driver instead of rotating it.
            if (n2 <= 1.0e-8f)
            {
                memcpy(o, bc, 16);
                continue;
            }
            const float inv = 1.0f / sqrtf(n2);
            for (int k = 0; k < 4; ++k)
                r[k] *= inv;
        }
        else
        {
            bool jump = false;
            for (int k = 0; k < 3; ++k)
            {
                const float dv = c[k] - p[k];
                if (dv > 5.0f || dv < -5.0f)     // a respawn, not motion
                    jump = true;
            }
            if (jump)
            {
                memcpy(o, bc, 16);
                continue;
            }
            for (int k = 0; k < 4; ++k)
                r[k] = p[k] + (c[k] - p[k]) * a;
        }
        memcpy(o, r, 16);
        blended += 4;
    }
    if (blended == 0)
        return;
    __try
    {
        memcpy(d.at, d.out, nb);
        d.wrote = true;
        d.outValid = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        d.have = false;
    }
}

void RestoreBlock(AnchorBlock& d)
{
    if (!d.wrote)
        return;
    d.wrote = false;
    if (!d.at)
    {
        d.outValid = false;
        return;
    }
    __try
    {
        // A value the sim rewrote after the blend is newer than our truth.
        for (uint32_t vi = 0; vi < d.vecs; ++vi)
        {
            float* at = d.at + vi * 4;
            if (memcmp(at, &d.out[vi * 4], 16) == 0)
                memcpy(at, &d.cur[vi * 4], 16);
        }
        d.outValid = false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        d.have = false;
        d.outValid = false;
    }
}
} // namespace

void ApplyAnchors(float a)
{
    for (size_t i = 0; i < kSlots; ++i)
        for (size_t j = 0; j < kBlocks; ++j)
            g_anchor[i][j].wrote = false;
    uint8_t* mgr = Manager();
    if (!mgr)
        return;

    for (size_t i = 0; i < kSlots; ++i)
    {
        AnchorBlock* s = g_anchor[i];
        if (!SlotActive(mgr, i))
        {
            s[0].have = false;
            s[1].have = false;
            continue;
        }
        uint32_t id;
        __try
        {
            id = *reinterpret_cast<const uint32_t*>(mgr + kStride * i + 3188);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            continue;
        }
        if (id == 0xFFFFFFFFu)
            continue;

        float* xf1 = nullptr;
        float* xf2 = nullptr;
        if (!ResolveAnchors(id, &xf1, &xf2))
        {
            s[0].have = false;
            s[1].have = false;
            continue;
        }
        // Exactly what sub_534990 reads: four float4s of the first handle,
        // two of the second. The array length is not stored anywhere.
        float* ptr[kBlocks] = { xf1, xf2 };
        const uint32_t vecs[kBlocks] = { 4, 2 };
        for (size_t j = 0; j < kBlocks; ++j)
        {
            AnchorBlock& d = s[j];
            if (!ptr[j])
            {
                d.have = false;
                continue;
            }
            if (d.at != ptr[j] || d.vecs != vecs[j])
            {
                d.at = ptr[j];             // reallocated, or another car in this slot
                d.vecs = vecs[j];
                d.have = false;
                d.outValid = false;
            }
            BlendBlock(d, a);
        }
    }
}

void RestoreAnchors()
{
    for (size_t i = 0; i < kSlots; ++i)
        for (size_t j = 0; j < kBlocks; ++j)
            RestoreBlock(g_anchor[i][j]);
}

void Compose()
{
    if (!g_composeOn)
        return;
    uint8_t* mgr = Manager();
    if (!mgr)
        return;
    __try
    {
        reinterpret_cast<ComposeFn>(kComposeFn)(mgr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_composeOn = false;             // one fault is enough: stop calling in
        LOG_ERROR("character composition faulted; per-frame driver update disabled");
    }
}

// The pose is produced by an async job (AnimationJob) once per tick. Blended
// at the render dispatch, after the worker drain, so the job has finished.
void ApplyPose(float a)
{
    for (size_t i = 0; i < kSlots; ++i)
        g_pose[i].wrote = false;
    uint8_t* mgr = Manager();
    if (!mgr)
        return;

    for (size_t i = 0; i < kSlots; ++i)
    {
        PoseState& d = g_pose[i];
        if (!SlotActive(mgr, i))
        {
            d.have = false;
            continue;
        }
        uint8_t* at = mgr + kPoseOff + kPoseBytes * i;
        uint32_t v[kPoseDw];
        __try
        {
            memcpy(v, at, kPoseBytes);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            d.have = false;
            continue;
        }
        if (!d.have)
        {
            memcpy(d.cur, v, kPoseBytes);
            memcpy(d.prev, v, kPoseBytes);
            d.have = true;
            continue;
        }
        const bool ours = d.wrote && memcmp(v, d.out, kPoseBytes) == 0;
        if (!ours && memcmp(v, d.cur, kPoseBytes) != 0)
        {
            memcpy(d.prev, d.cur, kPoseBytes);
            memcpy(d.cur, v, kPoseBytes);
        }
        if (memcmp(d.prev, d.cur, kPoseBytes) == 0)
            continue;

        long n = 0;
        for (size_t k = 0; k < kPoseDw; ++k)
        {
            const uint32_t bp = d.prev[k];
            const uint32_t bc = d.cur[k];
            if (bp == bc || !blend::Lerpable(bp) || !blend::Lerpable(bc))
            {
                d.out[k] = bc;
                continue;
            }
            float fp, fc;
            memcpy(&fp, &bp, sizeof(fp));
            memcpy(&fc, &bc, sizeof(fc));
            // More than a metre in one tick is a pose reset or a reused slot.
            const float step = fc - fp;
            if (step > 1.0f || step < -1.0f)
            {
                d.out[k] = bc;
                continue;
            }
            const float fo = fp + step * a;
            memcpy(&d.out[k], &fo, sizeof(fo));
            ++n;
        }
        if (n == 0)
            continue;
        __try
        {
            memcpy(at, d.out, kPoseBytes);
            d.wrote = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            d.have = false;
        }
    }
}

void RestorePose()
{
    uint8_t* mgr = Manager();
    if (!mgr)
        return;
    for (size_t i = 0; i < kSlots; ++i)
    {
        PoseState& d = g_pose[i];
        if (!d.wrote)
            continue;
        d.wrote = false;
        __try
        {
            memcpy(mgr + kPoseOff + kPoseBytes * i, d.cur, kPoseBytes);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            d.have = false;
        }
    }
}
} // namespace character
