#include "readouts.h"
#include "xform.h"
#include "objects.h"
#include "player.h"
#include "../patch.h"

#include <cstring>

namespace readouts
{
namespace
{
constexpr uint32_t kNodeBytes = 592;            // one pool node; nothing past it is ours
constexpr uint32_t kBins      = kNodeBytes / 4;
constexpr uint32_t kMaxNodes  = 64;
constexpr long     kStale     = 4;              // ticks without an update: not live
// A node is a placement when at least this many of its dwords are being
// rewritten every tick; static HUD containers never get there.
constexpr uint32_t kQualifyDw = 6;
// A dword is blended while it changed on most of the last 8 ticks.
constexpr uint8_t  kHistMin   = 6;
constexpr uint32_t kAnchorBin = 480 / 4;        // world anchor x, y, z

struct Node
{
    uint32_t at;
    bool     have;
    bool     qualified;
    long     lastTick;          // last tick the writer touched it
    long     ticks;             // samples taken
    uint32_t cur[kBins];
    uint32_t prev[kBins];
    uint32_t out[kBins];
    uint8_t  wrote[kBins];
    uint8_t  hist[kBins];       // changed or not, over the last 8 ticks
};
Node     g_nodes[kMaxNodes];
uint32_t g_nodeN = 0;
long     g_tick = 0;
bool     g_ready = false;

uint8_t PopCount(uint8_t v)
{
    static const uint8_t kNib[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
    return static_cast<uint8_t>(kNib[v & 0xF] + kNib[v >> 4]);
}

// The evidence a dword needs: n of n ticks while the node is young, six of
// eight once it has history.
uint8_t HistNeed(long ticks)
{
    if (ticks < 2)
        return 2;
    return static_cast<uint8_t>(ticks < kHistMin ? ticks : kHistMin);
}

// Pool nodes are reused across readouts, so a node's age says nothing about
// the readout on it: a dword rewritten on each of the last two ticks is a live
// placement either way.
bool Live(const Node& o, uint32_t k)
{
    return (o.hist[k] & 3u) == 3u || PopCount(o.hist[k]) >= HistNeed(o.ticks);
}

void Register(uint32_t at)
{
    if (at < 0x10000 || (at & 3) != 0)
        return;
    for (uint32_t i = 0; i < g_nodeN; ++i)
        if (g_nodes[i].at == at)
        {
            Node& o = g_nodes[i];
            if (o.lastTick < g_tick - kStale)
            {
                // The pool recycles addresses: re-snapshot so nothing is ever
                // blended between two different readouts.
                o.have = false;
                memset(o.wrote, 0, sizeof(o.wrote));
                memset(o.out, 0, sizeof(o.out));
            }
            o.lastTick = g_tick;
            return;
        }
    __try
    {
        volatile uint32_t t = *reinterpret_cast<const uint32_t*>(at);
        (void)t;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    uint32_t idx = g_nodeN;
    if (idx >= kMaxNodes)
    {
        uint32_t oldest = 0;
        long best = g_nodes[0].lastTick;
        for (uint32_t i = 1; i < kMaxNodes; ++i)
            if (g_nodes[i].lastTick < best)
            {
                best = g_nodes[i].lastTick;
                oldest = i;
            }
        if (best > g_tick - kStale)
            return;                              // all live: no room
        idx = oldest;
    }
    else
    {
        ++g_nodeN;
    }
    Node& o = g_nodes[idx];
    o.at = at;
    o.have = false;
    o.ticks = 0;
    o.qualified = false;
    o.lastTick = g_tick;
    memset(o.hist, 0, sizeof(o.hist));
    memset(o.wrote, 0, sizeof(o.wrote));
    memset(o.out, 0, sizeof(o.out));
}

// ---------------------------------------------------------------------------
// The node writer
// ---------------------------------------------------------------------------
// The call at 0x791818 (sub_791790 -> sub_7B53C0) is replaced. The UI may
// update off the game thread, so the hook only records the node; Run()
// registers it on the game thread.
constexpr uintptr_t kNodeSite   = 0x791818;
constexpr uintptr_t kNodeWriter = 0x7B53C0;
constexpr uint32_t  kSeenSlots  = 512;
volatile LONG g_seen[kSeenSlots];

using WriterFn = int(__fastcall*)(void*, void*, int, int);

int __fastcall NodeWriterHook(void* node, void* edx, int a2, int a3)
{
    const int r = reinterpret_cast<WriterFn>(kNodeWriter)(node, edx, a2, a3);
    const uint32_t at = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(node));
    InterlockedExchange(&g_seen[(at / kNodeBytes) % kSeenSlots], static_cast<LONG>(at));
    return r;
}

void RegisterSeen()
{
    for (uint32_t i = 0; i < kSeenSlots; ++i)
    {
        const uint32_t at = static_cast<uint32_t>(InterlockedExchange(&g_seen[i], 0));
        if (!at)
            continue;
        float p[3];
        __try
        {
            memcpy(p, reinterpret_cast<const void*>(at + 480), sizeof(p));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            continue;
        }
        // In-world nodes carry a world anchor; 2D HUD nodes have z exactly 0.
        if (!(p[2] > -1.0e5f && p[2] < 1.0e5f) || p[2] == 0.0f ||
            !(p[0] > -1.0e5f && p[0] < 1.0e5f))
            continue;
        Register(at);
    }
}

// Tick frames, after the subsystem pass: the state the engine composed for
// this tick.
void Sample()
{
    ++g_tick;
    for (uint32_t i = 0; i < g_nodeN; ++i)
    {
        Node& o = g_nodes[i];
        if (o.lastTick < g_tick - kStale)
            continue;
        const uint32_t* src = reinterpret_cast<const uint32_t*>(o.at);
        __try
        {
            if (!o.have)
            {
                memcpy(o.cur, src, sizeof(o.cur));
                memcpy(o.prev, src, sizeof(o.prev));
                o.have = true;
                continue;
            }
            ++o.ticks;
            if (!o.qualified)
            {
                uint32_t live = 0;
                for (uint32_t k = 0; k < kBins; ++k)
                    if (Live(o, k))
                        ++live;
                if (live >= kQualifyDw)
                    o.qualified = true;
            }
            for (uint32_t k = 0; k < kBins; ++k)
            {
                const uint32_t v = src[k];
                o.hist[k] = static_cast<uint8_t>(o.hist[k] << 1);
                if (v == o.cur[k])
                    continue;
                o.hist[k] |= 1u;
                // Our own write, left behind because a restore did not run,
                // is not a new value.
                if (o.wrote[k] && v == o.out[k])
                {
                    o.hist[k] = static_cast<uint8_t>(o.hist[k] & ~1u);
                    continue;
                }
                o.prev[k] = o.cur[k];
                o.cur[k] = v;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            o.lastTick = -1;
        }
    }
}
} // namespace

bool InstallNodeHook()
{
    return patch::RedirectCall(kNodeSite, kNodeWriter,
                               reinterpret_cast<const void*>(&NodeWriterHook));
}

void Run(bool tickFrame)
{
    if (!g_ready)
    {
        g_ready = true;
        return;
    }
    if (tickFrame)
    {
        RegisterSeen();
        Sample();
    }
}

void Apply(float a)
{
    if (!g_ready)
        return;

    // A readout that just appeared has no history yet. Its anchor is the
    // player car's tick position copied into the node, so it can follow the
    // car's own blend from the first frame: offset by (a - 1) * (cur - prev)
    // of the car while the anchor sits within 6 m of the car.
    float carStep[3] = { 0.0f, 0.0f, 0.0f };
    const float* carCur = nullptr;
    const float* carPrev = nullptr;
    const float* carNow = nullptr;
    if (objects::TickPair(player::CarObject(), &carPrev, &carNow))
    {
        for (int i = 0; i < 3; ++i)
            carStep[i] = (carNow[12 + i] - carPrev[12 + i]) * (a - 1.0f);
        carCur = carNow + 12;
    }

    for (uint32_t i = 0; i < g_nodeN; ++i)
    {
        Node& o = g_nodes[i];
        if (!o.have || o.lastTick < g_tick - kStale || !o.qualified)
            continue;
        uint32_t* mem = reinterpret_cast<uint32_t*>(o.at);
        __try
        {
            for (uint32_t k = 0; k < kBins; ++k)
            {
                o.wrote[k] = 0;
                if (o.prev[k] == o.cur[k])
                    continue;
                if (!Live(o, k))
                {
                    if (carCur && k >= kAnchorBin && k < kAnchorBin + 3 && mem[k] == o.cur[k] &&
                        blend::Lerpable(o.cur[k]))
                    {
                        float an[3];
                        memcpy(an, &o.cur[kAnchorBin], sizeof(an));
                        const float dx = an[0] - carCur[0], dy = an[1] - carCur[1],
                                    dz = an[2] - carCur[2];
                        if (dx * dx + dy * dy + dz * dz < 36.0f)
                        {
                            float c;
                            memcpy(&c, &o.cur[k], sizeof(c));
                            const float v = c + carStep[k - kAnchorBin];
                            memcpy(&o.out[k], &v, sizeof(o.out[k]));
                            mem[k] = o.out[k];
                            o.wrote[k] = 1;
                        }
                    }
                    continue;
                }
                if (!blend::Lerpable(o.prev[k]) || !blend::Lerpable(o.cur[k]))
                    continue;
                if (mem[k] != o.cur[k] && mem[k] != o.out[k])
                    continue;                    // the game rewrote it: leave it
                float p, c;
                memcpy(&p, &o.prev[k], sizeof(p));
                memcpy(&c, &o.cur[k], sizeof(c));
                const float v = p + (c - p) * a;
                memcpy(&o.out[k], &v, sizeof(o.out[k]));
                mem[k] = o.out[k];
                o.wrote[k] = 1;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            o.lastTick = -1;
        }
    }
}

void Restore()
{
    if (!g_ready)
        return;
    for (uint32_t i = 0; i < g_nodeN; ++i)
    {
        Node& o = g_nodes[i];
        uint32_t* mem = reinterpret_cast<uint32_t*>(o.at);
        __try
        {
            for (uint32_t k = 0; k < kBins; ++k)
            {
                if (!o.wrote[k])
                    continue;
                o.wrote[k] = 0;
                if (mem[k] == o.out[k])
                    mem[k] = o.cur[k];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            o.lastTick = -1;
        }
    }
}
} // namespace readouts
