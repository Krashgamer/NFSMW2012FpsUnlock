#include "frame.h"
#include "character.h"
#include "objects.h"
#include "player.h"
#include "readouts.h"
#include "view.h"
#include "../barrier.h"
#include "../clock.h"
#include "../log.h"
#include "../patch.h"

extern "C" uintptr_t g_gameObject = 0;   // written by the stubs below

namespace frame
{
namespace
{
uint32_t g_frame = 0;
float    g_alpha = 0.0f;
bool     g_snap = false;          // a catch-up batch ran: show current state
bool     g_window = false;
bool     g_beganEarly = false;    // the pre-subsystem path did this frame's work
DWORD    g_gameTid = 0;

int64_t g_phaseBase = 0;
int64_t g_periodEff = 0;
int     g_lastDue = 1;
bool    g_phaseValid = false;

// How far this frame is into the current tick period.
float ComputeAlpha()
{
    if (!g_phaseValid || g_periodEff <= 0)
        return 1.0f;
    const int64_t since = Now100ns() - g_phaseBase;
    if (since < 0)
        return 0.0f;
    if (since > 3 * g_periodEff)
        return 1.0f;                     // ticks stalled (pause, hitch): hold
    const float a = static_cast<float>(static_cast<double>(since) /
                                       static_cast<double>(g_periodEff));
    return a > 1.0f ? 1.0f : a;
}

void BeginFrameWork()
{
    g_gameTid = GetCurrentThreadId();
    ++g_frame;
    g_snap = g_lastDue > 1;
    g_alpha = ComputeAlpha();
    player::BeginFrame();
    objects::BeginFrame(Alpha());
    g_window = true;
    objects::Prune();
}

// Runs on the game thread right before the subsystem pass (0x476FA5).
void OnPreSubsystems()
{
    // A tick runs inside the subsystem pass and must be handed truth.
    if (g_lastDue != 0)
        return;
    BeginFrameWork();
    // The character composition below reads these, a few hundred
    // instructions from here: this is the only window that reaches it.
    character::ApplyAnchors(Alpha());
    // Last, after every blend it reads.
    character::Compose();
    objects::Reapply();
    view::Apply(Alpha());
    readouts::Run(false);
    readouts::Apply(Alpha());
    g_beganEarly = true;
}

// Start of the render phase (0x476FAC). Does the work on tick frames, where
// the pre-subsystem path had to step aside.
void OnRenderBegin()
{
    if (g_beganEarly)
    {
        g_beganEarly = false;
        return;
    }
    BeginFrameWork();
    // The engine composed the driver inside the tick, from truth, while the
    // car renders from the blend. Re-run the composition so every frame is
    // built the same way.
    character::ApplyAnchors(Alpha());
    character::Compose();
    objects::Reapply();
    view::Apply(Alpha());
    readouts::Run(true);
    readouts::Apply(Alpha());
}

// The camera / render dispatch call (0x46F987).
void OnRenderDispatch()
{
    character::ApplyPose(Alpha());
}

// Right after the render controller returns (0x46FA22).
void OnRenderEnd()
{
    g_window = false;
    view::Restore();
    objects::Restore();
    character::RestorePose();
    character::RestoreAnchors();
}

// ---------------------------------------------------------------------------
// Call-site hooks
// ---------------------------------------------------------------------------
// All four sites are `call` instructions in the game's frame and render
// functions. Each stub drains the render workers first (the blends write
// memory the workers read), then chains to whatever the call currently
// targets - another ASI may already have taken the site, and that hook keeps
// working in either load order.
constexpr uintptr_t kSubsysCall   = 0x476FA5;   // call sub_4766A0
constexpr uintptr_t kPhaseCall    = 0x476FAC;   // call sub_46F730
constexpr uintptr_t kDispatchCall = 0x46F987;   // call sub_8C7FE0
constexpr uintptr_t kRenderCall   = 0x46FA1D;   // call sub_8CFFA0
constexpr uintptr_t kRenderRet    = 0x46FA22;

const uint8_t kMovEcxEsiCall[3] = { 0x8B, 0xCE, 0xE8 };
const uint8_t kMovEcxEbpCall[3] = { 0x8B, 0xCD, 0xE8 };

// mov [g_gameObject], ecx / push ecx / call drain / call fn / pop ecx /
// jmp target. The call already pushed the return address.
void HookPhaseCall(uintptr_t site, void (*fn)())
{
    const uintptr_t target = patch::CallTarget(site);
    uint8_t* mem = patch::AllocCode(64);
    patch::Stub s{ mem };
    s.B(0x89); s.B(0x0D); s.D32(reinterpret_cast<uint32_t>(&g_gameObject));
    s.B(0x51);
    s.Call(reinterpret_cast<const void*>(&WaitRenderDrain));
    s.Call(reinterpret_cast<const void*>(fn));
    s.B(0x59);
    s.Jmp(target);
    patch::Branch(site, 0xE8, mem);
}

void HookDispatchCall()
{
    const uintptr_t target = patch::CallTarget(kDispatchCall);
    uint8_t* mem = patch::AllocCode(32);
    patch::Stub s{ mem };
    s.B(0x51);                                                          // push ecx
    s.Call(reinterpret_cast<const void*>(&OnRenderDispatch));
    s.B(0x59);                                                          // pop ecx
    s.Jmp(target);
    patch::Branch(kDispatchCall, 0xE8, mem);
}

// The render controller is wrapped rather than prefixed: drain, run it, drain
// again (the jobs it dispatched must finish before the blends come out), take
// the object and view blends back out, continue after the call.
void HookRenderCall()
{
    const uintptr_t target = patch::CallTarget(kRenderCall);
    uint8_t* mem = patch::AllocCode(64);
    patch::Stub s{ mem };
    const uintptr_t post = reinterpret_cast<uintptr_t>(mem) + 0x20;
    s.B(0x51);                                                          // push ecx
    s.Call(reinterpret_cast<const void*>(&WaitRenderDrain));
    s.B(0x59);                                                          // pop ecx
    s.B(0x68); s.D32(static_cast<uint32_t>(post));                      // push post
    s.Jmp(target);
    patch::Stub p{ mem + 0x20 };
    p.Call(reinterpret_cast<const void*>(&WaitRenderDrain));
    p.Call(reinterpret_cast<const void*>(&OnRenderEnd));
    p.Jmp(kRenderRet);
    // `call` becomes `jmp`: the stub supplies its own return address.
    patch::Branch(kRenderCall, 0xE9, mem);
}
} // namespace

bool SitesOk()
{
    return patch::Matches(kSubsysCall - 2, kMovEcxEsiCall, 3) &&
           patch::Matches(kPhaseCall - 2, kMovEcxEsiCall, 3) &&
           patch::Matches(kDispatchCall - 2, kMovEcxEbpCall, 3) &&
           patch::Matches(kRenderCall - 2, kMovEcxEbpCall, 3);
}

void Install()
{
    HookRenderCall();
    HookDispatchCall();
    HookPhaseCall(kPhaseCall, &OnRenderBegin);
    HookPhaseCall(kSubsysCall, &OnPreSubsystems);
}

void SetPhase(int64_t phaseBase, int64_t periodEff, int lastDue, bool valid)
{
    g_phaseBase = phaseBase;
    g_periodEff = periodEff > 0 ? periodEff : 0;
    g_lastDue = lastDue;
    g_phaseValid = valid;
}

void Restore()
{
    g_window = false;
    view::Restore();
    readouts::Restore();
    objects::Restore();
    character::RestorePose();
    character::RestoreAnchors();
}

uint32_t  Index()      { return g_frame; }
float     Alpha()      { return g_snap ? 1.0f : g_alpha; }
float     RawAlpha()   { return g_alpha; }
bool      InWindow()   { return g_window; }
DWORD     GameThread() { return g_gameTid; }
uintptr_t GameObject() { return g_gameObject; }
} // namespace frame
