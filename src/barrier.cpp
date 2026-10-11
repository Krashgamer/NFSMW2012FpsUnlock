#include "barrier.h"
#include "clock.h"
#include "patch.h"

namespace
{
// Jobs currently running. sub_DA07A0 is the job system's single job invoker:
// every job of every queue runs inside it, so a counter around it is an exact
// "jobs in flight" signal, including workers blocked in a syscall mid-job.
volatile LONG g_activeJobs = 0;

constexpr uintptr_t kJobInvoker = 0xDA07A0;
constexpr uint8_t kJobInvokerPrologue[5] = { 0x55, 0x8B, 0xEC, 0x51, 0x56 };
//                                           push ebp; mov ebp,esp; push ecx; push esi

// Jobs queued but not yet picked up. The counter above reads 0 while the
// queue is full, so it cannot close the gap between submission and pickup on
// its own. The engine keeps that count itself: sub_D9F640 (the queue pop)
// tests [queue+4] and decrements it with `lock xadd`. The worker loop
// sub_DA3710 pops from the queue at [jobSystem+8], so the depth lives at
// jobSystem+12. The job system pointer is [ecx+4] of the worker loop's
// argument; it is latched once per worker thread at the loop's entry.
volatile uintptr_t g_jobSys = 0;

constexpr uintptr_t kWorkerLoop = 0xDA3710;
constexpr uint8_t kWorkerLoopPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };

// A queue that has not drained within 4 ms is not this frame's render work
// (streaming, audio, ...). Holding the game thread for it would only cost
// frame rate, so past this bound the drain falls back to the running count.
constexpr int64_t kQueueWaitMax100 = 40000;

// Absolute safety bound on the running-job wait; never reached in practice.
constexpr int64_t kMaxWait100 = 1000000000;

bool InstallJobInvokerHook()
{
    if (!patch::Matches(kJobInvoker, kJobInvokerPrologue, sizeof(kJobInvokerPrologue)))
        return false;
    uint8_t* mem = patch::AllocCode(64);
    if (!mem)
        return false;

    // Wrapper: lock inc; call the original with its single stack argument;
    // lock dec (flags and return value preserved); retn 4.
    patch::Stub s{ mem };
    const uintptr_t trampoline = reinterpret_cast<uintptr_t>(mem) + 40;
    s.LockInc(&g_activeJobs);
    s.B(0x8B); s.B(0x44); s.B(0x24); s.B(0x04);          // mov eax, [esp+4]
    s.B(0x50);                                           // push eax
    s.B(0xB8); s.D32(static_cast<uint32_t>(trampoline)); // mov eax, trampoline
    s.B(0xFF); s.B(0xD0);                                // call eax
    s.B(0x9C); s.B(0x50); s.B(0x52);                     // pushfd; push eax; push edx
    s.LockDec(&g_activeJobs);
    s.B(0x5A); s.B(0x58); s.B(0x9D);                     // pop edx; pop eax; popfd
    s.B(0xC2); s.B(0x04); s.B(0x00);                     // retn 4

    // Trampoline: the stolen prologue, then back into the function.
    patch::Stub t{ mem + 40 };
    t.Bytes(kJobInvokerPrologue, sizeof(kJobInvokerPrologue));
    t.Jmp(kJobInvoker + sizeof(kJobInvokerPrologue));

    patch::Jump(kJobInvoker, mem);
    return true;
}

bool InstallJobSysHook()
{
    if (!patch::Matches(kWorkerLoop, kWorkerLoopPrologue, sizeof(kWorkerLoopPrologue)))
        return false;
    uint8_t* mem = patch::AllocCode(64);
    if (!mem)
        return false;
    patch::Stub s{ mem };
    s.B(0x8B); s.B(0x41); s.B(0x04);                                 // mov eax, [ecx+4]
    s.B(0xA3); s.D32(reinterpret_cast<uint32_t>(&g_jobSys));         // mov [g_jobSys], eax
    s.Bytes(kWorkerLoopPrologue, sizeof(kWorkerLoopPrologue));
    s.Jmp(kWorkerLoop + sizeof(kWorkerLoopPrologue));
    patch::Detour(kWorkerLoop, mem, sizeof(kWorkerLoopPrologue));
    return true;
}
} // namespace

bool BarrierSitesOk()
{
    return patch::Matches(kJobInvoker, kJobInvokerPrologue, sizeof(kJobInvokerPrologue)) &&
           patch::Matches(kWorkerLoop, kWorkerLoopPrologue, sizeof(kWorkerLoopPrologue));
}

bool InstallBarrierHooks()
{
    return BarrierSitesOk() && InstallJobSysHook() && InstallJobInvokerHook();
}

void WaitRenderDrain()
{
    const int64_t start = Now100ns();
    int spins = 0;
    while (g_activeJobs != 0)
    {
        SwitchToThread();
        if (++spins >= 4)
            Sleep(0);
        if (Now100ns() - start > kMaxWait100)
            return;
    }

    // Both have to hold at the same time: nothing left to pick up, and
    // nothing still running.
    const uintptr_t js = g_jobSys;
    if (js)
    {
        volatile LONG* depth = reinterpret_cast<volatile LONG*>(js + 12);
        if (*depth != 0 || g_activeJobs != 0)
        {
            const int64_t qStart = Now100ns();
            while (*depth != 0 || g_activeJobs != 0)
            {
                SwitchToThread();
                if (Now100ns() - qStart > kQueueWaitMax100)
                    break;
            }
        }
    }
    else
    {
        // No worker has started yet: one yield, then the running count.
        SwitchToThread();
        while (g_activeJobs != 0)
        {
            SwitchToThread();
            if (Now100ns() - start > kMaxWait100)
                break;
        }
    }
}

void EndFrameBackpressure()
{
    SwitchToThread();
}
