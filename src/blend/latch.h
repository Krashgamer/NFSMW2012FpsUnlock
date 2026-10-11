#pragma once
#include "xform.h"
#include "../pacer.h"

#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace blend
{
// Blends a matrix the game hands to a draw call, keyed by something stable
// (the part it belongs to). The matrix is only latched, never written back:
// the draw gets a blended copy.
//
// Per key, prev/cur are latched whenever the matrix changes. The copy is
// drawn at LerpTransform(prev, cur, frame alpha), except when
//   - it did not change across the last tick (stopped: draw cur),
//   - it jumped more than 5 m (a reset: draw cur),
//   - it changed twice within one tick (already updated per frame from
//     blended inputs: hands off for two seconds).
// Called from render worker threads, hence the lock.
template <int kSlots>
class TickLatch
{
public:
    // Returns the matrix to draw: `m` itself, or `out` holding the blend.
    const float* Blend(uintptr_t key, const float* m, float* out, float alpha)
    {
        const float* use = m;
        if (!key)
            return use;
        const long frameNo = FrameCount();
        AcquireSRWLockExclusive(&m_lock);
        const long seq = TickCount();
        Entry* e = Find(key, frameNo);
        if (e)
        {
            if (e->latchSeq < 0)
            {
                memcpy(e->cur, m, sizeof(e->cur));
                e->latchSeq = seq;
            }
            else if (memcmp(e->cur, m, sizeof(e->cur)) != 0)
            {
                if (e->havePrev && e->latchSeq == seq)
                    e->perFrameUntil = frameNo + 120;
                memcpy(e->prev, e->cur, sizeof(e->prev));
                memcpy(e->cur, m, sizeof(e->cur));
                e->havePrev = true;
                e->latchSeq = seq;
            }
            e->lastFrame = frameNo;
            if (e->havePrev && frameNo >= e->perFrameUntil && seq == e->latchSeq)
            {
                const float jx = e->cur[12] - e->prev[12], jy = e->cur[13] - e->prev[13],
                            jz = e->cur[14] - e->prev[14];
                if (jx * jx + jy * jy + jz * jz <= 25.0f)
                {
                    LerpTransform(out, e->prev, e->cur, alpha);
                    use = out;
                }
            }
        }
        ReleaseSRWLockExclusive(&m_lock);
        return use;
    }

private:
    struct Entry
    {
        uintptr_t key;
        float cur[16], prev[16];
        long latchSeq, lastFrame, perFrameUntil;
        bool havePrev;
    };

    // Open addressing; an entry unseen for 600 frames may be taken over.
    Entry* Find(uintptr_t key, long frameNo)
    {
        const uint32_t h = static_cast<uint32_t>((key >> 4) * 2654435761u) & (kSlots - 1);
        Entry* stale = nullptr;
        for (int i = 0; i < 32; ++i)
        {
            Entry& e = m_entries[(h + i) & (kSlots - 1)];
            if (e.key == key)
                return &e;
            if (e.key == 0 || frameNo - e.lastFrame > 600)
            {
                if (!stale)
                    stale = &e;
                if (e.key == 0)
                    break;
            }
        }
        if (!stale)
            return nullptr;
        memset(stale, 0, sizeof(*stale));
        stale->key = key;
        stale->latchSeq = -1;
        return stale;
    }

    Entry   m_entries[kSlots] = {};
    SRWLOCK m_lock = SRWLOCK_INIT;
};
} // namespace blend
