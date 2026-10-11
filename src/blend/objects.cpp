#include "objects.h"
#include "frame.h"
#include "xform.h"
#include "player.h"
#include "../patch.h"

#include <cstring>

namespace objects
{
namespace
{
constexpr size_t   kMapSlots = 4096;         // open addressing on the object pointer
constexpr size_t   kMaxLerped = 2048;        // writes per frame that can be restored
constexpr uint32_t kEvictCars = 90;          // frames unseen before a car is dropped
// Everything that is not a car comes and goes constantly; a freed object's
// memory must not keep receiving 64-byte writes. Their getter runs every
// frame they are alive, so a short window costs nothing.
constexpr uint32_t kEvictTransient = 12;
constexpr float    kTeleport2 = 25.0f * 25.0f;   // a jump this far is not motion

struct ObjState
{
    void*    obj;            // key; nullptr = free slot
    void*    tptr;           // the 64-byte transform
    void*    vt;             // vtable at registration: a reused allocation has another
    float    prev[16];       // previous tick
    float    cur[16];        // current tick (truth, restored at frame end)
    float    out[16];        // what this frame wrote
    uint32_t lastSeenFrame;
    uint32_t fetchedFrame;   // frame this object was last blended
    uint32_t outFrame;       // frame out[] belongs to
    float    lastAlpha;      // a lower alpha without a new tick = the period wrapped
    uint8_t  gx;             // which getter registered it (0 = cars)
};

ObjState  g_map[kMapSlots];
ObjState* g_lerped[kMaxLerped];
size_t    g_lerpedN = 0;

inline uint32_t EvictFrames(uint8_t gx) { return gx == 0 ? kEvictCars : kEvictTransient; }

inline size_t HashPtr(const void* p)
{
    return (static_cast<size_t>(reinterpret_cast<uintptr_t>(p)) >> 4) * 2654435761u &
           (kMapSlots - 1);
}

ObjState* FindState(const void* obj)
{
    const size_t start = HashPtr(obj);
    for (size_t probe = 0; probe < kMapSlots; ++probe)
    {
        ObjState& s = g_map[(start + probe) & (kMapSlots - 1)];
        if (s.obj == obj)
            return &s;
    }
    return nullptr;
}

ObjState* InsertState(void* obj)
{
    const size_t start = HashPtr(obj);
    for (size_t probe = 0; probe < kMapSlots; ++probe)
    {
        ObjState& s = g_map[(start + probe) & (kMapSlots - 1)];
        if (s.obj == obj)
            return &s;
        if (s.obj == nullptr || frame::Index() - s.lastSeenFrame > kEvictCars)
        {
            s.obj = obj;
            s.tptr = nullptr;
            s.lastSeenFrame = 0;
            s.fetchedFrame = 0;
            memset(s.prev, 0, sizeof(s.prev));
            memset(s.cur, 0, sizeof(s.cur));
            return &s;
        }
    }
    return nullptr;
}

// The object must still BE the object: checked before every read or write.
bool VtOk(const ObjState& s)
{
    if (!s.obj || !s.vt)
        return true;
    void* now = nullptr;
    __try
    {
        now = *reinterpret_cast<void**>(s.obj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return now == s.vt;
}

bool OnGameThread()
{
    const DWORD tid = frame::GameThread();
    return tid == 0 || GetCurrentThreadId() == tid;
}

// ---------------------------------------------------------------------------
// Worker-thread sightings
// ---------------------------------------------------------------------------
// The map is game-thread only. A getter call on another thread is still a
// sighting, so it is queued and registered at the start of the next frame.
// Bounded and lossy: a full queue drops.
constexpr uint32_t kFqMax = 512;
struct FqEntry { void* obj; void* t; uint8_t gx; };
FqEntry g_fq[kFqMax];
volatile LONG g_fqHead = 0;

void FqPush(void* obj, void* t, int gx)
{
    if (!obj || !t)
        return;
    const LONG i = InterlockedIncrement(&g_fqHead) - 1;
    if (i < 0 || static_cast<uint32_t>(i) >= kFqMax)
        return;
    g_fq[i].obj = obj;
    g_fq[i].t = t;
    g_fq[i].gx = static_cast<uint8_t>(gx);
}

// ---------------------------------------------------------------------------
// Registration and blending
// ---------------------------------------------------------------------------
// Registration only, for a getter call outside the blend window: that is the
// engine reading truth, so its tick states are latched and the frame-start
// pass blends the object from then on.
void Register(void* obj, void* t, int gx)
{
    if (!t)
        return;
    if (!OnGameThread())
        return;
    ObjState* s = FindState(obj);
    bool fresh = false;
    if (!s)
    {
        s = InsertState(obj);
        fresh = s != nullptr;
    }
    if (!s)
        return;
    float v[16];
    __try
    {
        memcpy(v, t, sizeof(v));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s->obj = nullptr;
        return;
    }
    if (!blend::Plausible(v))
    {
        s->obj = nullptr;
        return;
    }
    if (fresh)
    {
        s->gx = static_cast<uint8_t>(gx);
        __try
        {
            s->vt = *reinterpret_cast<void**>(obj);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s->obj = nullptr;
            return;
        }
        s->tptr = t;
        memcpy(s->cur, v, sizeof(s->cur));
        memcpy(s->prev, v, sizeof(s->prev));
        s->lastAlpha = -1.0f;
    }
    s->lastSeenFrame = frame::Index();
    if (s->tptr != t)
    {
        // Storage moved (e.g. a traffic car handed to physics). Keep the
        // history when it is plainly the same object.
        s->tptr = t;
        if (blend::PosDist2(v, s->cur) > kTeleport2)
            memcpy(s->prev, v, sizeof(s->prev));
        else
            memcpy(s->prev, s->cur, sizeof(s->prev));
        memcpy(s->cur, v, sizeof(s->cur));
    }
    else if (memcmp(v, s->cur, sizeof(v)) != 0)
    {
        if (blend::PosDist2(v, s->cur) > kTeleport2)
        {
            memcpy(s->prev, v, sizeof(s->prev));
            memcpy(s->cur, v, sizeof(s->cur));
        }
        else
        {
            memcpy(s->prev, s->cur, sizeof(s->prev));
            memcpy(s->cur, v, sizeof(s->cur));
        }
        s->lastAlpha = -1.0f;
    }
}

// A getter call inside the blend window. Objects the frame-start pass already
// blended return straight away; a new one is registered and blended here.
void* TrackLerp(void* obj, void* t, int gx)
{
    if (!t || !frame::InWindow())
        return t;
    if (!OnGameThread())
    {
        FqPush(obj, t, gx);
        return t;
    }

    ObjState* s = FindState(obj);
    bool fresh = false;
    if (!s)
    {
        s = InsertState(obj);
        fresh = s != nullptr;
    }
    if (!s)
        return t;                        // map full: stays at 60 Hz
    if (fresh)
    {
        s->gx = static_cast<uint8_t>(gx);
        __try
        {
            s->vt = *reinterpret_cast<void**>(obj);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s->obj = nullptr;
            return t;
        }
    }

    s->lastSeenFrame = frame::Index();
    if (s->fetchedFrame == frame::Index())
        return t;                        // already blended this frame
    s->fetchedFrame = frame::Index();

    if (s->tptr != t)
    {
        // First sighting, or the transform moved to other storage: carry the
        // history across a move when the contents are the same object.
        const bool firstSight = s->tptr == nullptr;
        float nv[16];
        __try
        {
            memcpy(nv, t, sizeof(nv));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s->obj = nullptr;
            return t;
        }
        if (!blend::Plausible(nv))
        {
            s->obj = nullptr;
            return t;
        }
        s->tptr = t;
        if (!firstSight && blend::PosDist2(nv, s->cur) <= kTeleport2)
        {
            memcpy(s->prev, s->cur, sizeof(s->prev));
            memcpy(s->cur, nv, sizeof(s->cur));
            return t;
        }
        memcpy(s->cur, nv, sizeof(s->cur));
        memcpy(s->prev, nv, sizeof(s->prev));
        return t;
    }

    float v[16];
    __try
    {
        memcpy(v, t, sizeof(v));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s->obj = nullptr;
        return t;
    }
    if (!blend::Plausible(v))
    {
        s->obj = nullptr;
        return t;
    }

    const float raw = frame::RawAlpha();
    if (memcmp(v, s->cur, sizeof(v)) != 0)
    {
        if (blend::PosDist2(v, s->cur) > kTeleport2)
        {
            memcpy(s->prev, v, sizeof(s->prev));   // respawn / reset: snap
            memcpy(s->cur, v, sizeof(s->cur));
            return t;
        }
        memcpy(s->prev, s->cur, sizeof(s->prev));
        memcpy(s->cur, v, sizeof(s->cur));
        s->lastAlpha = -1.0f;
    }
    else if (raw < s->lastAlpha)
    {
        memcpy(s->prev, s->cur, sizeof(s->prev));  // paused: converge
    }
    s->lastAlpha = raw;

    float out[16];
    blend::LerpTransform(out, s->prev, s->cur, frame::Alpha());
    __try
    {
        memcpy(s->tptr, out, sizeof(out));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s->obj = nullptr;
        return t;
    }
    if (g_lerpedN < kMaxLerped)
        g_lerped[g_lerpedN++] = s;
    return t;
}

// ---------------------------------------------------------------------------
// Getter hooks
// ---------------------------------------------------------------------------
// sub_41E220 (`lea eax,[ecx+0E0h]; ret`) is the getter of every car.
void* __fastcall CarGetterHook(void* obj, void* /*edx*/)
{
    char* t = static_cast<char*>(obj) + 224;
    player::NoteCarTransform(obj);
    return TrackLerp(obj, t, 0);
}

// The other trivial getters: `lea eax,[ecx+OFF]; ret`.
template <int OFF, int IDX>
void* __fastcall GetterHook(void* obj, void* /*edx*/)
{
    char* t = static_cast<char*>(obj) + OFF;
    if (!frame::InWindow())
    {
        Register(obj, t, IDX);
        return t;
    }
    return TrackLerp(obj, t, IDX);
}

// The shared-array classes (constructed in sub_425BD0, the world pool): a
// 16-byte record whose matrix lives in an array owned by something else.
//
//   sub_41CD60  [[this+8]+0x28] + [this+0xC]
//   sub_41CC00  [[this+8]+0x28] + [this+0xC] + 0x20
//   0x547450    [this+0x18] & 2 ? this+0x20 : [[this+0xC]+0x28] + [this+0x10]
//
// The arithmetic is the original's, unguarded: the loads are the ones the
// original makes, so a fault behaves as it would have.
enum { kShA, kShB, kShC };

template <int KIND, int IDX>
void* __fastcall SharedGetterHook(void* obj, void* /*edx*/)
{
    char* o = static_cast<char*>(obj);
    char* t;
    if (KIND == kShC)
    {
        if ((*reinterpret_cast<uint8_t*>(o + 0x18) & 2) != 0)
            t = o + 0x20;
        else
            t = *reinterpret_cast<char**>(*reinterpret_cast<char**>(o + 0x0C) + 0x28) +
                *reinterpret_cast<int32_t*>(o + 0x10);
    }
    else
    {
        t = *reinterpret_cast<char**>(*reinterpret_cast<char**>(o + 8) + 0x28) +
            *reinterpret_cast<int32_t*>(o + 0x0C) + (KIND == kShB ? 0x20 : 0);
    }
    if (!frame::InWindow())
    {
        Register(obj, t, IDX);
        return t;
    }
    return TrackLerp(obj, t, IDX);
}

struct GetterSite
{
    uintptr_t addr;
    uint8_t   body[10];   // the exact getter (plus the 0xCC padding the jmp lands in)
    size_t    len;
    void*     hook;
};
} // namespace

int InstallGetterHooks()
{
    static const GetterSite kSites[] = {
        { 0x41E220, { 0x8D, 0x81, 0xE0, 0x00, 0x00, 0x00, 0xC3 }, 7,
          reinterpret_cast<void*>(&CarGetterHook) },
        { 0x41AEF0, { 0x8D, 0x41, 0x10, 0xC3, 0xCC }, 5,
          reinterpret_cast<void*>(&GetterHook<0x010, 1>) },
        { 0x41CF30, { 0x8D, 0x41, 0x10, 0xC3, 0xCC }, 5,
          reinterpret_cast<void*>(&GetterHook<0x010, 2>) },
        { 0x41E4F0, { 0x8D, 0x41, 0x10, 0xC3, 0xCC }, 5,
          reinterpret_cast<void*>(&GetterHook<0x010, 3>) },
        { 0x4202D0, { 0x8D, 0x41, 0x30, 0xC3, 0xCC }, 5,
          reinterpret_cast<void*>(&GetterHook<0x030, 4>) },
        { 0x532C20, { 0x8D, 0x41, 0x30, 0xC3, 0xCC }, 5,
          reinterpret_cast<void*>(&GetterHook<0x030, 5>) },
        { 0x420640, { 0x8D, 0x81, 0x10, 0x01, 0x00, 0x00, 0xC3 }, 7,
          reinterpret_cast<void*>(&GetterHook<0x110, 6>) },
        { 0x41D960, { 0x8D, 0x41, 0x10, 0xC3, 0xCC }, 5,
          reinterpret_cast<void*>(&GetterHook<0x010, 10>) },
        // mov eax,[ecx+8]; mov eax,[eax+28]; add eax,[ecx+0C]; ret
        { 0x41CD60, { 0x8B, 0x41, 0x08, 0x8B, 0x40, 0x28, 0x03, 0x41, 0x0C, 0xC3 }, 10,
          reinterpret_cast<void*>(&SharedGetterHook<kShA, 7>) },
        // ... lea eax,[edx+eax+20]; ret
        { 0x41CC00, { 0x8B, 0x41, 0x08, 0x8B, 0x50, 0x28, 0x8B, 0x41, 0x0C, 0x8D }, 10,
          reinterpret_cast<void*>(&SharedGetterHook<kShB, 8>) },
        // test byte [ecx+18],2; jnz +0A; ...
        { 0x547450, { 0xF6, 0x41, 0x18, 0x02, 0x75, 0x0A, 0x8B, 0x41, 0x0C, 0x8B }, 10,
          reinterpret_cast<void*>(&SharedGetterHook<kShC, 9>) },
    };
    int n = 0;
    for (const GetterSite& e : kSites)
    {
        if (!patch::Matches(e.addr, e.body, e.len))
            continue;
        patch::Jump(e.addr, e.hook);
        ++n;
    }
    return n;
}

void BeginFrame(float a)
{
    const uint32_t frameNo = frame::Index();

    // Sightings from worker threads during the frame just gone, so the pass
    // below sees them this frame rather than next.
    {
        const LONG n = InterlockedExchange(&g_fqHead, 0);
        const uint32_t take = n < 0 ? 0u : (static_cast<uint32_t>(n) > kFqMax ? kFqMax : static_cast<uint32_t>(n));
        for (uint32_t i = 0; i < take; ++i)
            Register(g_fq[i].obj, g_fq[i].t, g_fq[i].gx);
    }

    // If the previous frame never restored (a fault path), restore now so no
    // blended value can be mistaken for a tick.
    if (g_lerpedN != 0)
        Restore();

    // Blend every tracked object now, at the start of the window. The world
    // render pass reads many transforms directly from the object without
    // calling the getter, so a getter-time blend alone would miss them.
    for (size_t k = 0; k < kMapSlots; ++k)
    {
        ObjState* s = &g_map[k];
        if (!s->obj || !s->tptr)
            continue;
        if (!VtOk(*s))
        {
            s->obj = nullptr;
            continue;
        }
        // Memory holds truth here: last frame's restore put it back, and this
        // frame's ticks ran before the render phase.
        float v[16];
        __try
        {
            memcpy(v, s->tptr, sizeof(v));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s->obj = nullptr;
            continue;
        }
        if (!blend::Plausible(v))
        {
            s->obj = nullptr;
            continue;
        }
        if (memcmp(v, s->cur, sizeof(v)) != 0)
        {
            if (blend::PosDist2(v, s->cur) > kTeleport2)
            {
                memcpy(s->prev, v, sizeof(s->prev));   // teleport: snap
                memcpy(s->cur, v, sizeof(s->cur));
                s->lastAlpha = a;
                s->fetchedFrame = frameNo;
                continue;
            }
            memcpy(s->prev, s->cur, sizeof(s->prev));
            memcpy(s->cur, v, sizeof(s->cur));
            s->lastAlpha = -1.0f;
        }
        else if (a < s->lastAlpha)
        {
            memcpy(s->prev, s->cur, sizeof(s->prev));  // period wrapped, no tick
        }
        s->lastAlpha = a;

        if (memcmp(s->prev, s->cur, sizeof(s->prev)) == 0)
        {
            s->fetchedFrame = frameNo;                  // static: nothing to blend
            continue;
        }
        // A write that cannot be restored would be read back next frame as a
        // tick and corrupt the history for good.
        if (g_lerpedN >= kMaxLerped)
            continue;

        float out[16];
        blend::LerpTransform(out, s->prev, s->cur, a);
        memcpy(s->out, out, sizeof(s->out));
        s->outFrame = frameNo;
        __try
        {
            memcpy(s->tptr, out, sizeof(out));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s->obj = nullptr;
            continue;
        }
        s->fetchedFrame = frameNo;
        g_lerped[g_lerpedN++] = s;
    }
}

void Prune()
{
    const uint32_t frameNo = frame::Index();
    for (size_t i = 0; i < kMapSlots; ++i)
    {
        ObjState& s = g_map[i];
        if (s.obj && s.tptr && frameNo - s.lastSeenFrame > EvictFrames(s.gx))
            s.obj = nullptr;
    }
}

void Reapply()
{
    // Only the two small-prop classes: they sit in the composition's node
    // list. Cars must keep what the composition wrote.
    const uint32_t frameNo = frame::Index();
    for (size_t k = 0; k < g_lerpedN; ++k)
    {
        ObjState* s = g_lerped[k];
        if (!s->obj || !s->tptr || s->outFrame != frameNo || (s->gx != 2 && s->gx != 3))
            continue;
        __try
        {
            memcpy(s->tptr, s->out, sizeof(s->out));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s->obj = nullptr;
        }
    }
}

void Restore()
{
    for (size_t k = 0; k < g_lerpedN; ++k)
    {
        ObjState* s = g_lerped[k];
        if (!s->obj)
            continue;
        if (!VtOk(*s))
        {
            s->obj = nullptr;            // freed between the blend and here
            continue;
        }
        __try
        {
            memcpy(s->tptr, s->cur, sizeof(s->cur));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s->obj = nullptr;
        }
    }
    g_lerpedN = 0;
}

bool TickPair(const void* obj, const float** prev, const float** cur)
{
    const ObjState* s = obj ? FindState(obj) : nullptr;
    if (!s || !s->tptr)
        return false;
    *prev = s->prev;
    *cur = s->cur;
    return true;
}
} // namespace objects
