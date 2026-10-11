#include "crashguard.h"
#include "guards.h"
#include "log.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------------------
// Fault emulation
// ---------------------------------------------------------------------------
// Removing the 60 Hz frame pacing lets the game thread and the jobified render
// bucket workers observe render objects mid-construction. Pointer fields still
// hold constructor defaults (1 / 1.0f / 0xCDCDCDCD) and the garbage propagates
// through arbitrary callees. On an access violation in game code, the guard
// decodes the faulting instruction and emulates it as if the memory operand
// held 0:
//   memory reads            -> destination register gets 0
//   ALU op, register dest   -> compute with memory operand 0, flags set
//   ALU op, memory dest     -> flags as if memory were 0, write skipped
//   LOCK-prefixed RMW       -> same (xadd: old value 0; xchg: reg = 0;
//                              inc/dec/cmpxchg analogous), write skipped
//   memory writes           -> skipped (memory left as-is for this frame)
// Faults outside game .text, in instructions the decoder does not classify, or
// with prefixes it does not model (segment / rep on non-string ops) are never
// emulated: they are written to the log and passed on to the system.

namespace
{

volatile LONG g_hitCount = 0;

LONG WINAPI CrashGuardHandler(PEXCEPTION_POINTERS ep);  // fwd

// NFS13.exe v1.5 image (decrypted, no ASLR, base 0x400000).
// .text section: RVA 0x1000 .. 0xB81E81 ==> absolute 0x401000 .. 0xF81E81.
constexpr uint32_t kTextStart = 0x401000;
constexpr uint32_t kTextEnd   = 0xF81E81;

// x86 CONTEXT: XMM0-7 live in the FXSAVE area of ExtendedRegisters at
// offset 0x16C, 16 bytes each.
constexpr size_t kXmmOffset = 0x16C;

// ---------------------------------------------------------------------------
// Crash report
// ---------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable: 4613)
bool SafeRead32(const void* p, uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const volatile uint32_t*>(p);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SafeReadBytes(const void* p, void* out, size_t n)
{
    __try
    {
        memcpy(out, p, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// "module.dll+0xoffset" for any address.
void FormatModuleAddr(char* out, size_t n, uint32_t addr)
{
    HMODULE h = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &h) || !h)
    {
        _snprintf_s(out, n, _TRUNCATE, "0x%08lX", static_cast<unsigned long>(addr));
        return;
    }
    char path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(h, path, MAX_PATH);
    const char* name = "?";
    if (len > 0 && len < MAX_PATH)
    {
        name = path;
        for (DWORD i = len; i-- > 0;)
            if (path[i] == '\\' || path[i] == '/')
            {
                name = path + i + 1;
                break;
            }
    }
    _snprintf_s(out, n, _TRUNCATE, "%s+0x%lX", name,
                static_cast<unsigned long>(addr - reinterpret_cast<uintptr_t>(h)));
}

bool InOwnModule(uint32_t addr)
{
    static uintptr_t lo = 0, hi = 0;
    if (!lo)
    {
        HMODULE self{};
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CrashGuardHandler), &self);
        const auto base = reinterpret_cast<uintptr_t>(self);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        hi = base + nt->OptionalHeader.SizeOfImage;
        lo = base;
    }
    return addr >= lo && addr < hi;
}

// At most a few full reports per session: an exception that repeats is the
// same report again, and the log must stay small enough to attach.
constexpr LONG kMaxReports = 8;
volatile LONG g_reports = 0;

void LogFault(const EXCEPTION_POINTERS* ep)
{
    if (!LogEnabled(LogLevel::Error))
        return;
    if (InterlockedIncrement(&g_reports) > kMaxReports)
        return;

    const PCONTEXT c = ep->ContextRecord;
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    char eipMod[128];
    FormatModuleAddr(eipMod, sizeof(eipMod), c->Eip);
    LOG_ERROR("exception 0x%08lX at %s (thread %lu), emulated faults so far: %ld",
              r->ExceptionCode, eipMod, GetCurrentThreadId(), g_hitCount);

    char buf[256];
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
    {
        const char* kind = r->ExceptionInformation[0] == 0 ? "read"
                         : r->ExceptionInformation[0] == 1 ? "write"
                         : r->ExceptionInformation[0] == 8 ? "execute" : "?";
        char target[128];
        FormatModuleAddr(target, sizeof(target),
                         static_cast<uint32_t>(r->ExceptionInformation[1]));
        _snprintf_s(buf, _TRUNCATE, "  access violation: %s at %s\r\n", kind, target);
        LogRaw(buf);
    }
    _snprintf_s(buf, _TRUNCATE,
                "  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX\r\n"
                "  esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX efl=%08lX\r\n",
                c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip,
                c->EFlags);
    LogRaw(buf);

    uint8_t bytes[16]{};
    if (SafeReadBytes(reinterpret_cast<const void*>(c->Eip), bytes, sizeof(bytes)))
    {
        int n = _snprintf_s(buf, _TRUNCATE, "  code:");
        for (int i = 0; i < 16 && n > 0; i++)
            n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, " %02X", bytes[i]);
        strcat_s(buf, "\r\n");
        LogRaw(buf);
    }

    // Frame-pointer chain.
    LogRaw("  call stack (ebp chain):\r\n");
    uint32_t ebp = c->Ebp;
    for (int depth = 0; depth < 20 && ebp >= 0x10000 && ebp < 0x80000000u; ++depth)
    {
        uint32_t next = 0, ret = 0;
        if (!SafeRead32(reinterpret_cast<const void*>(ebp), &next) ||
            !SafeRead32(reinterpret_cast<const void*>(ebp + 4), &ret))
            break;
        char mod[128];
        FormatModuleAddr(mod, sizeof(mod), ret);
        _snprintf_s(buf, _TRUNCATE, "    %s\r\n", mod);
        LogRaw(buf);
        if (next <= ebp)
            break;
        ebp = next;
    }

    // Return-address candidates on the stack, for frames without ebp.
    LogRaw("  stack scan:\r\n");
    uint32_t sp = c->Esp;
    for (int i = 0, shown = 0; i < 512 && shown < 32; ++i, sp += 4)
    {
        uint32_t w = 0;
        if (!SafeRead32(reinterpret_cast<const void*>(sp), &w))
            break;
        if (w < 0x401000u || w >= 0x80000000u || (w >= c->Esp && w < c->Esp + 0x1000))
            continue;
        HMODULE h = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(w), &h) || !h)
            continue;
        char mod[128];
        FormatModuleAddr(mod, sizeof(mod), w);
        _snprintf_s(buf, _TRUNCATE, "    +0x%04X %s\r\n", i * 4, mod);
        LogRaw(buf);
        ++shown;
    }
}

// The first emulation at each address is logged once, so a report can say
// where the torn reads happen without the log growing with every frame.
void LogEmulatedFault(uint32_t eip, const EXCEPTION_RECORD* r)
{
    static uint8_t seen[(kTextEnd - kTextStart) / 4]{};
    uint8_t& s = seen[(eip - kTextStart) / 4];
    if (s)
        return;
    s = 1;
    const char* kind = r->NumberParameters >= 2
        ? (r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "exec")
        : "?";
    LOG_VERBOSE("crash guard: emulated %s fault at 0x%08lX (target 0x%08lX)", kind,
                static_cast<unsigned long>(eip),
                r->NumberParameters >= 2 ? static_cast<unsigned long>(r->ExceptionInformation[1]) : 0ul);
}
#pragma warning(pop)

enum Action
{
    kUnusable,
    kZero32,      // mov r32, m / xchg r32, m   -> r = 0
    kZero8,       // mov r8,  m / xchg r8,  m   -> r8 = 0
    kZeroXmm,     // movaps xmm, m              -> xmm = 0
    kComiXmm,     // comiss/ucomiss xmm, m      -> flags(xmm.low vs 0.0)
    kOr8FF,       // or  r8,  m                 -> r8 |= 0xFF
    kOr32FF,      // or  r32, m                 -> r  |= 0xFFFFFFFF
    kAndZero,     // and r32, m                 -> r = 0
    kAdd0,        // add/sub/xor r32, m         -> r unchanged, flags(r)
    kSkipWrite,   // mov m, r / movaps m, xmm   -> skip
    kCmpRm0,      // cmp r, m                   -> flags(r - 0)
    kCmpNegR,     // cmp m, r                   -> flags(0 - r)
    kTest0,       // test m, r                  -> flags(0)
    kMAdd,        // add/adc m, r               -> flags(0 + r), skip write
    kMOr,         // or  m, r                   -> flags(r),     skip write
    kMAnd,        // and m, r                   -> flags(0),     skip write
    kMSub,        // sub/sbb/cmp m, r           -> flags(0 - r), skip write
    kMXor,        // xor m, r                   -> flags(r),     skip write
    kXadd,        // xadd m, r                  -> r = 0, flags(0 + r)
    kIncM,        // inc m                      -> flags(1), CF kept
    kDecM,        // dec m                      -> flags(-1), CF kept
    kCmpxchg,     // cmpxchg m, r               -> cmp acc with 0, acc=0 if !=
    kRepString,   // rep movs/stos/lods/cmps/scas -> complete as no-op, ECX=0
    kAluImm,      // add/or/adc/sbb/and/sub/xor/cmp m, imm -> flags(0 op imm)
    kX87Skip,     // fadd/fsub m                -> st0 +- 0, nothing to do
    kX87Zero,     // fmul/fdiv/fdivr m          -> st0 = 0
    kX87Neg,      // fsubr m                    -> st0 = 0 - st0
    kX87Com,      // fcom m                     -> C3/C2/C0 from st0 : 0
    kX87ComP,     // fcomp m                    -> ... and pop
    kX87Push0,    // fld/fild m                 -> push 0.0
    kX87Pop,      // fstp/fistp m               -> skip write, pop
    kZeroFlags,   // neg m / shift m / test m,imm -> flags(0), write skipped
    kMulM,        // mul/imul m                 -> EDX:EAX = 0
    kDivM,        // div/idiv m                 -> quotient 0, remainder 0
    kCmov,        // cmovcc r32, m              -> r = 0 iff the condition holds
    kCmpx8,       // cmpxchg8b m                -> compare EDX:EAX against 0
    kStrOne,      // one-shot movs/stos/lods/cmps/scas (no REP)
    kPopM,        // pop m                      -> skip write, ESP += 4
};

struct Decoded
{
    int     len;
    Action  action;
    uint8_t reg;      // destination/operand GPR (kInPlace = none)
    bool    eight;    // 8-bit operand form
    bool    sixteen;  // 0x66 operand-size prefix: 16-bit width
    bool    imm8;     // trailing imm8 (SSE pshuf/cmp/shuf forms)
    uint8_t aluop;    // ALU op for kAluImm (0=add..7=cmp)
    uint32_t imm;     // immediate operand for kAluImm
    uint8_t immBytes; // trailing immediate after the memory operand (C7/F7/69)
    uint8_t sub;      // cmovcc cc, string opcode, or "0x83 sign-extends" flag
    bool    high;     // 8-bit operand is ah/ch/dh/bh, not al/cl/dl/bl
};

constexpr uint8_t kInPlace = 0xFF;

// ---------------------------------------------------------------------------
// x87, inside the FXSAVE block of CONTEXT (ExtendedRegisters, offset 0xCC):
// FCW at 0, FSW at 2, abridged FTW at 4, ST(i) at 32 + 16*physical. kXmmOffset
// is that same block + 160, and writing XMM through it already works, so the
// kernel does honour this area on resume.
//
// The x87 escape opcodes (D8-DF) are emulated too: `fadd dword ptr [esi+1Ch]`
// on a 0xCDCDCDCD constructor default is a real fault of the game's render
// code. The memory operand reads as 0.0, same contract as every integer form
// here.
constexpr size_t kFxOffset = kXmmOffset - 160;


// ---------------------------------------------------------------------------
// The remaining instruction classes, and why these and not others.
//
// Coverage is checked statically: every register-based memory access in .text
// disassembled and run through this decoder. Of 1188045 such instructions,
// the most common shapes a narrower decoder would reject are these:
//
//   53297  C7  mov m32, imm32        14260  C6  mov m8, imm8
//    3161  F6/F7 test m, imm          1488  66 C7 mov m16, imm16
//     596  F7 div/idiv/mul m           577  0F AF imul r32, m
//     489  D9 /5 fldcw m16             330  FE inc/dec m8
//    ~1000  02/12/13/1A/1B/22/2A/32 (8-bit and adc/sbb r, m)
//     141  0F 90-9F setcc m8           120  C0-D3 shift m
//      82  0F C7 cmpxchg8b m          ~500  A4-AF one-shot string ops
//
// In sub_550020 alone - the render bucket builder, where torn render objects
// fault - there are 32 C7 and 22 C6 sites, plus 4 imul, 1 div and 3 setcc.
// `mov byte ptr [eax+edx], 1` at 0x550252 is the same shape as the slot write
// the bounds guards cover.
//
// NOT added, deliberately: FF /6 `push m`. The only correct emulation writes
// the pushed dword at ESP-4, and on x86 the kernel builds the exception record
// and CONTEXT on the faulting thread's stack immediately BELOW the faulting
// ESP - so that write lands in the structures NtContinue is about to read.
// FF /2 and FF /4 (call/jmp through memory) stay rejected: there is no value
// to continue with, the transfer target is the thing that faulted.

// CONTEXT carries the FPU twice on x86: the legacy FloatSave block at 0x1C
// (FNSAVE format) and the FXSAVE block at 0xCC. Which one the kernel applies
// last on resume is not documented, and a discarded push would desync the FPU
// stack in a way that is far worse than the fault it replaced. So write both,
// and keep them agreeing. Physical register order is the same in each; only
// the stride (10 vs 16) and the tag encoding differ.
constexpr size_t kFsOffset = 0x1C;    // CONTEXT.FloatSave

uint8_t* FxBase(PCONTEXT ctx)
{
    return reinterpret_cast<uint8_t*>(ctx) + kFxOffset;
}

uint8_t* FsBase(PCONTEXT ctx)
{
    return reinterpret_cast<uint8_t*>(ctx) + kFsOffset;
}

int FpuTop(PCONTEXT ctx)
{
    uint16_t fsw = 0;
    memcpy(&fsw, FxBase(ctx) + 2, sizeof(fsw));
    return (fsw >> 11) & 7;
}

void FpuSetStatus(PCONTEXT ctx, uint16_t fsw)
{
    memcpy(FxBase(ctx) + 2, &fsw, sizeof(fsw));
    uint32_t wide = fsw;
    memcpy(FsBase(ctx) + 0x04, &wide, sizeof(wide));
}

uint16_t FpuStatus(PCONTEXT ctx)
{
    uint16_t fsw = 0;
    memcpy(&fsw, FxBase(ctx) + 2, sizeof(fsw));
    return fsw;
}

void FpuSetTop(PCONTEXT ctx, int top)
{
    const uint16_t fsw = FpuStatus(ctx);
    FpuSetStatus(ctx, static_cast<uint16_t>(
        (fsw & 0xC7FFu) | static_cast<uint16_t>((top & 7) << 11)));
}

// FXSAVE: one bit per physical register, 1 = valid. FNSAVE: two bits,
// 01 = zero, 11 = empty. The only value this emulator ever stores is +0.0.
void FpuTagValid(PCONTEXT ctx, int phys, bool valid)
{
    uint8_t* t = FxBase(ctx) + 4;
    *t = static_cast<uint8_t>(valid ? (*t | (1u << phys))
                                    : (*t & ~(1u << phys)));
    uint32_t tw = 0;
    memcpy(&tw, FsBase(ctx) + 0x08, sizeof(tw));
    tw &= ~(3u << (2 * phys));
    tw |= (valid ? 1u : 3u) << (2 * phys);
    memcpy(FsBase(ctx) + 0x08, &tw, sizeof(tw));
}

uint8_t* FpuSlot(PCONTEXT ctx, int phys)
{
    return FxBase(ctx) + 32 + 16 * phys;
}

void FpuWrite0(PCONTEXT ctx, int phys)
{
    memset(FpuSlot(ctx, phys), 0, 16);              // +0.0 in 80-bit is zeroes
    memset(FsBase(ctx) + 0x1C + 10 * phys, 0, 10);
}

void FpuPush0(PCONTEXT ctx)
{
    const int top = (FpuTop(ctx) + 7) & 7;   // a push decrements TOP
    FpuWrite0(ctx, top);
    FpuTagValid(ctx, top, true);
    FpuSetTop(ctx, top);
}

void FpuPop(PCONTEXT ctx)
{
    const int top = FpuTop(ctx);
    FpuTagValid(ctx, top, false);
    FpuSetTop(ctx, (top + 1) & 7);
}

void FpuSt0Zero(PCONTEXT ctx)
{
    const int top = FpuTop(ctx);
    FpuWrite0(ctx, top);
    FpuTagValid(ctx, top, true);
}

void FpuNegSt0(PCONTEXT ctx)
{
    const int top = FpuTop(ctx);
    uint8_t* x = FpuSlot(ctx, top);
    x[9] = static_cast<uint8_t>(x[9] ^ 0x80);   // sign is the top bit of byte 9
    uint8_t* f = FsBase(ctx) + 0x1C + 10 * top;
    f[9] = static_cast<uint8_t>(f[9] ^ 0x80);
}

// C3 (0x4000) / C2 (0x0400) / C0 (0x0100): equal 100, less 001, greater 000.
void FpuComZero(PCONTEXT ctx)
{
    const uint8_t* s = FpuSlot(ctx, FpuTop(ctx));
    bool zero = true;
    for (int i = 0; i < 10; ++i)
    {
        const uint8_t m = (i == 9) ? 0x7F : 0xFF;
        if (s[i] & m)
        {
            zero = false;
            break;
        }
    }
    const bool neg = (s[9] & 0x80) != 0;
    uint16_t fsw = static_cast<uint16_t>(FpuStatus(ctx) & ~0x4500u);
    if (zero)
        fsw = static_cast<uint16_t>(fsw | 0x4000u);
    else if (neg)
        fsw = static_cast<uint16_t>(fsw | 0x0100u);
    FpuSetStatus(ctx, fsw);
}


bool ParseMemOperand(const uint8_t* p, int pos, int* len)
{
    const uint8_t modrm = p[pos];
    const uint8_t mod = modrm >> 6;
    const uint8_t rm  = modrm & 7;

    if (mod == 0 && rm == 5)
        return false;               // [disp32] absolute: not register-based

    *len = pos + 1;
    if (rm == 4)                    // SIB byte
    {
        const uint8_t sib = p[pos + 1];
        ++*len;
        if (mod == 0 && (sib & 7) == 5)
            *len += 4;              // base=101 with mod=00: disp32 after SIB
    }
    if (mod == 1) *len += 1;        // disp8
    if (mod == 2) *len += 4;        // disp32
    return true;
}

// Classify a two-byte (0F) SSE opcode by memory direction.
// Returns 0 = not SSE, 1 = xmm<-memory load, 2 = memory<-xmm store,
// 3 = GPR<-memory convert (cvtts s2si). mandatoryPrefix means the opcode
// arrived with 66/F2/F3 (SSE2 integer / scalar forms); without it, most
// 0x6x/0xDx+ opcodes are MMX (mm regs) and are left unclassified.
// 4 = flags from comparing the xmm register against memory-as-0.
// Fault-path semantics: a load reads "memory held 0" (zero xmm / zero GPR),
// a store is skipped entirely. comiss/ucomiss produce real flags, and they
// are reproducible: the xmm operand is in the CONTEXT and the memory operand
// is 0 like every other emulated read (`comiss xmm0, [eax+ecx*4-4]` at
// 0x8A03BA is a real fault site).
int ClassifySse(uint8_t op, bool mandatoryPrefix, bool* hasImm)
{
    *hasImm = false;
    switch (op)
    {
    case 0x2E: case 0x2F:
        return 4;                                   // ucomiss/comiss (66: sd)
    case 0x10: case 0x12: case 0x14: case 0x15: case 0x16: case 0x2A:
    case 0x28:
        return 1;                                   // movups/movlps/.../movaps load
    case 0x11: case 0x13: case 0x17: case 0x29:
        return 2;                                   // stores
    case 0x2C: case 0x2D:
        return 3;                                   // cvttss2si/cvtss2si -> GPR
    case 0xC2: *hasImm = true; return 1;            // cmpps/cmpss/cmppd/cmpsd
    case 0xC6: *hasImm = true; return 1;            // shufps/shufpd
    case 0x70: *hasImm = true; return mandatoryPrefix ? 1 : 0; // pshufd/hw/lw
    case 0x7E:
        return mandatoryPrefix ? 1 : 0;             // movd/movq xmm, m (66/F3)
    case 0x6E:
        return mandatoryPrefix ? 1 : 0;             // movd xmm, m32 (66)
    case 0x7F:
        return mandatoryPrefix ? 2 : 0;             // movdqa/movdqu m, xmm
    case 0xD6:
        return mandatoryPrefix ? 2 : 0;             // movq m, xmm (66)
    case 0xE7:
        return mandatoryPrefix ? 2 : 0;             // movntdq m, xmm (66)
    default:
        if (op >= 0x51 && op <= 0x5F) return 1;     // sqrt/rsqrt/rcp/and/or/
                                                    // xor/add/mul/cvt/min/
                                                    // div/max/sub (mem source)
        if (mandatoryPrefix)
        {
            // SSE2 integer xmm forms (all memory sources except the stores
            // carved out above): punpck*, padd*, psub*, pcmp*, pmul*, pand...
            if (op >= 0x60 && op <= 0x6F && op != 0x67) return 1;
            if (op >= 0xD4 && op <= 0xFE) return 1;
        }
        return 0;
    }
}

// Returns true and fills d if the instruction at p is an emulable memory
// access through a register (any addressing mode, LOCK allowed).
bool Decode(const uint8_t* p, Decoded* d)
{
    d->len = 0;
    d->action = kUnusable;
    d->reg = kInPlace;
    d->eight = false;
    d->sixteen = false;
    d->imm8 = false;
    d->imm = 0;
    d->aluop = 0;
    d->immBytes = 0;
    d->sub = 0;
    d->high = false;

    int pos = 0;
    const uint8_t pfx = p[0];

    // REP-prefixed string ops (rep movs/stos/lods/cmps/scas) can't be
    // emulated as "memory held 0" -- instead model the transfer as already
    // complete with a zero element count. This covers the whole string
    // family so a poisoned memcpy (garbage ECX length) doesn't crash.
    {
        const uint8_t pre  = p[0];
        const int     pIdx = (pre == 0x66) ? 1 : 0;      // operand-size pfx
        const uint8_t rep  = p[pIdx];
        const uint8_t sOp  = p[pIdx + 1];
        if ((rep == 0xF3 || rep == 0xF2) && sOp >= 0xA4 && sOp <= 0xAF)
        {
            d->action = kRepString;
            d->len    = 2 + pIdx;
            return true;
        }
    }

    // The same family without a REP prefix: an inlined single movs/stos/lods/
    // cmps/scas. Modelled as "the element was not transferred", with the
    // pointers advanced exactly as the instruction would advance them, so the
    // surrounding loop still terminates.
    {
        const uint8_t sOp = (pfx == 0x66) ? p[1] : p[0];
        if ((sOp >= 0xA4 && sOp <= 0xA7) || (sOp >= 0xAA && sOp <= 0xAF))
        {
            d->action  = kStrOne;
            d->sub     = sOp;
            d->eight   = ((sOp & 1) == 0);
            d->sixteen = (pfx == 0x66);
            d->len     = (pfx == 0x66) ? 2 : 1;
            return true;
        }
    }

    if (pfx == 0xF0)
        pos = 1;                    // LOCK: decode the plain op
    else if (pfx == 0x66 || pfx == 0xF2 || pfx == 0xF3)
    {
        if (p[1] == 0x0F)
        {
            // SSE with mandatory prefix (66 = pd/integer, F3 = ss/du,
            // F2 = sd): classify direction and emulate. Loads read as
            // "memory held 0" (zero xmm - for movd/movq this is exactly
            // the architectural zero-extension), stores are skipped.
            bool hasImm = false;
            const int cls = ClassifySse(p[2], true, &hasImm);
            if (cls != 0)
            {
                const uint8_t modrm = p[3];
                if ((modrm >> 6) == 3)
                    return false;   // register form: not a memory fault
                int memLen = 0;
                if (!ParseMemOperand(p, 3, &memLen))
                    return false;
                d->len = memLen + (hasImm ? 1 : 0);
                if (cls == 1)
                {
                    d->action = kZeroXmm;
                    d->reg = (modrm >> 3) & 7;
                }
                else if (cls == 2)
                {
                    d->action = kSkipWrite;
                }
                else if (cls == 4)
                {
                    d->action = kComiXmm;         // comisd/ucomisd
                    d->reg = (modrm >> 3) & 7;
                    d->sixteen = (pfx == 0x66);   // double precision
                }
                else
                {
                    d->action = kZero32;          // cvtts s2si -> GPR
                    d->reg = (modrm >> 3) & 7;
                    d->sixteen = (pfx == 0x66);
                }
                return true;
            }
            // 0x66 0F B6-style (movzx r16, m8) etc: fall through to the
            // generic 16-bit path below; the SSE table did not match.
        }
        if (pfx != 0x66)
            return false;           // F2/F3 on integer ops: not modeled

        // 16-bit integer instruction (0x66 operand-size prefix, e.g.
        // `mov ax, [esi+0x24]` = 66 8B 46 24 - a real fault class seen in
        // the 03:07 crash). Decode the base 32-bit op, then adjust the
        // width: writes zero only the low 16 bits, flags compute at 16-bit
        // width, and the imm32 form shrinks to imm16 (instruction length!).
        if (!Decode(p + 1, d))
            return false;
        d->len += 1;
        d->sixteen = true;
        if (d->immBytes == 4)
        {
            // 0x81 / C7 / F7-test / 69: imm32 becomes imm16 under 0x66, and
            // that is an INSTRUCTION LENGTH, not a cosmetic detail. 0x83's
            // sign-extended imm8 does not shrink, which is why this is keyed
            // on the immediate's own size and not on the action.
            d->len -= 2;
            d->immBytes = 2;
            d->imm &= 0xFFFF;
        }
        return true;
    }
    else if (pfx == 0x67 || pfx == 0x26 || pfx == 0x2E || pfx == 0x36 ||
             pfx == 0x3E || pfx == 0x64 || pfx == 0x65)
        return false;               // segment/size/rep prefixes: not modeled

    const uint8_t op = p[pos];

    switch (op)
    {
    // ---- register-destination ops (memory read as 0) ----
    case 0x8B: d->action = kZero32;  pos += 1; break;   // mov r32, m
    case 0x8A: d->action = kZero8;   pos += 1; break;   // mov r8,  m
    case 0x87: d->action = kZero32;  pos += 1; break;   // xchg r32, m
    case 0x86: d->action = kZero8;   pos += 1; break;   // xchg r8,  m
    case 0x0A: d->action = kOr8FF;   pos += 1; break;   // or  r8,  m
    case 0x0B: d->action = kOr32FF;  pos += 1; break;   // or  r32, m
    case 0x23: d->action = kAndZero; pos += 1; break;   // and r32, m
    case 0x03:                                          // add r32, m
    case 0x2B:                                          // sub r32, m
    case 0x33: d->action = kAdd0;    pos += 1; break;   // xor r32, m
    // The 8-bit twins and the carry forms of the same group. adc/sbb with a
    // zero memory operand differ from add/sub only in the incoming CF, which
    // kAdd0 does not model - but the result register is unchanged either way
    // and the flags are within one carry bit of correct, which is the same
    // contract every other emulated form here runs on.
    case 0x02:                                          // add r8,  m
    case 0x2A:                                          // sub r8,  m
    case 0x32:                                          // xor r8,  m
    case 0x12:                                          // adc r8,  m
    case 0x1A: d->action = kAdd0;    pos += 1; d->eight = true;
               break;  // sbb r8,  m
    case 0x13:                                          // adc r32, m
    case 0x1B: d->action = kAdd0;    pos += 1;
               break;  // sbb r32, m
    case 0x22: d->action = kAndZero; pos += 1; d->eight = true;
               break;  // and r8,  m
    case 0x3A: d->action = kCmpRm0;  pos += 1; d->eight = true;  break;
    case 0x3B: d->action = kCmpRm0;  pos += 1; break;   // cmp r32, m
    case 0x38: d->action = kCmpNegR; pos += 1; d->eight = true;  break;
    case 0x39: d->action = kCmpNegR; pos += 1; break;   // cmp m, r32
    case 0x84: d->action = kTest0;   pos += 1; d->eight = true;  break;
    case 0x85: d->action = kTest0;   pos += 1; break;   // test m, r32

    // ---- memory-destination ALU (flags only, write skipped) ----
    case 0x00: case 0x01: case 0x10: case 0x11:        // add/adc m, r
        d->action = kMAdd; pos += 1; d->eight = ((op & 1) == 0); break;
    case 0x08: case 0x09:                               // or m, r
        d->action = kMOr;  pos += 1; d->eight = ((op & 1) == 0); break;
    case 0x20: case 0x21:                               // and m, r
        d->action = kMAnd; pos += 1; d->eight = ((op & 1) == 0); break;
    case 0x18: case 0x19: case 0x28: case 0x29:        // sbb/sub m, r
        d->action = kMSub; pos += 1; d->eight = ((op & 1) == 0); break;
    case 0x30: case 0x31:                               // xor m, r
        d->action = kMXor; pos += 1; d->eight = ((op & 1) == 0); break;

    // ---- plain writes ----
    case 0x89: d->action = kSkipWrite; pos += 1; break; // mov m, r32
    case 0x88: d->action = kSkipWrite; pos += 1; break; // mov m, r8
    // mov m, imm - 53297 sites in .text, 32 of them in sub_550020, and the
    // single largest hole this decoder had after the x87 group. Pure write:
    // same contract as `mov m, r`.
    case 0xC6: d->action = kSkipWrite; pos += 1; d->eight = true;
               d->immBytes = 1; break;
    case 0xC7: d->action = kSkipWrite; pos += 1; d->immBytes = 4;
               break;

    // ---- immediate-ALU group (add/or/adc/sbb/and/sub/xor/cmp r/m, imm) ----
    // Memory-destination; the read faults, so emulate flags as "memory was 0".
    case 0x80: d->action = kAluImm; d->eight = true;  pos += 1;
               d->immBytes = 1; break;                  // r/m8, imm8
    case 0x81: d->action = kAluImm; d->eight = false; pos += 1;
               d->immBytes = 4; break;                  // r/m32, imm32
    case 0x82: d->action = kAluImm; d->eight = true;  pos += 1;
               d->immBytes = 1; break;                  // r/m8, imm8 (alias)
    case 0x83: d->action = kAluImm; d->eight = false; pos += 1;
               d->immBytes = 1; d->sub = 1; break;      // r/m32, imm8 sign-ext

    case 0xFF:                                          // group: inc/dec m
    {
        const uint8_t reg = (p[pos + 1] >> 3) & 7;
        if (reg == 0)      d->action = kIncM;
        else if (reg == 1) d->action = kDecM;
        else return false;             // call/jmp/push m: see the banner
        pos += 1;
        break;
    }

    // 8F /0 pop m: the value comes off the stack, the write into memory is
    // what faulted. Skip the write, but ESP still has to move or the caller
    // returns through a shifted stack.
    case 0x8F:
    {
        if (((p[pos + 1] >> 3) & 7) != 0)
            return false;
        d->action = kPopM;
        pos += 1;
        break;
    }

    case 0xFE:                                          // group: inc/dec m8
    {
        const uint8_t reg = (p[pos + 1] >> 3) & 7;
        if (reg == 0)      d->action = kIncM;
        else if (reg == 1) d->action = kDecM;
        else return false;
        d->eight = true;
        pos += 1;
        break;
    }

    // F6/F7: test / not / neg / mul / imul / div / idiv, memory operand.
    // div and idiv are the interesting ones: the operand read as 0 would be a
    // divide by zero, i.e. a second, worse exception. The completion that
    // matches the rest of this emulator is quotient 0, remainder 0 - the
    // caller sees the same "the memory held nothing" result every other class
    // gives it.
    case 0xF6:
    case 0xF7:
    {
        const uint8_t reg = (p[pos + 1] >> 3) & 7;
        const bool    b8  = (op == 0xF6);
        switch (reg)
        {
        case 0: case 1:                                 // test m, imm
            d->action = kZeroFlags;
            d->immBytes = b8 ? 1 : 4;
            break;
        case 2: d->action = kSkipWrite;  break;         // not m: no flags
        case 3: d->action = kZeroFlags;  break;         // neg m: 0 - 0 = 0
        case 4: case 5: d->action = kMulM; break;       // mul / imul m
        case 6: case 7: d->action = kDivM; break;       // div / idiv m
        default: return false;
        }
        d->eight = b8;
        pos += 1;
        break;
    }

    // Shift/rotate with a memory destination. Memory reads as 0, so the result
    // is 0 for every form except rcl/rcr with CF set - and the write is
    // skipped in all of them, so only the flags are approximate.
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    {
        d->action = kZeroFlags;
        d->eight  = (op == 0xC0 || op == 0xD0 || op == 0xD2);
        if (op == 0xC0 || op == 0xC1)
            d->immBytes = 1;
        pos += 1;
        break;
    }

    // imul r32, m, imm - the memory operand is 0, so the product is 0.
    case 0x69: d->action = kAndZero; pos += 1; d->immBytes = 4; break;
    case 0x6B: d->action = kAndZero; pos += 1; d->immBytes = 1; break;

    case 0x0F:
    {
        const uint8_t op2 = p[pos + 1];
        switch (op2)
        {
        case 0xB6: d->action = kZero32;  pos += 2; break; // movzx r32, m8
        case 0xB7: d->action = kZero32;  pos += 2; break; // movzx r32, m16
        case 0xBE: d->action = kZero32;  pos += 2; break; // movsx r32, m8
        case 0xBF: d->action = kZero32;  pos += 2; break; // movsx r32, m16
        case 0xC0: d->action = kXadd; pos += 2; d->eight = true;  break;
        case 0xC1: d->action = kXadd; pos += 2; break;   // xadd m, r32
        case 0xB0: d->action = kCmpxchg; pos += 2; d->eight = true;  break;
        case 0xB1: d->action = kCmpxchg; pos += 2; break; // cmpxchg m, r32
        case 0xAF: d->action = kAndZero;   pos += 2;      // imul r32, m -> 0
                   break;
        case 0xAE: d->action = kSkipWrite; pos += 2;      // ldmxcsr/stmxcsr
                   break;
        // bt/bts/btr/btc read the bit out of memory: it is 0, so CF is 0 and
        // the write is skipped. bsf/bsr on a zero source set ZF and leave the
        // destination alone. Both are exactly "flags of 0".
        case 0xA3: case 0xAB: case 0xB3: case 0xBB:
        case 0xBC: case 0xBD: d->action = kZeroFlags; pos += 2; break;
        case 0xBA: d->action = kZeroFlags; pos += 2; d->immBytes = 1; break;
        case 0xC7:                                       // cmpxchg8b m64
        {
            if (((p[pos + 2] >> 3) & 7) != 1)
                return false;
            d->action = kCmpx8; pos += 2; break;
        }
        default:
        {
            // Unprefixed single-precision SSE (movups/movaps/movss-less
            // forms, ALU like addps/subps/mulps/divps, min/max/sqrt, and
            // cvttss2si to a GPR). subps xmm,[ecx+10h] (0F 5C 41 10) is a
            // real fault site.
            if (op2 >= 0x90 && op2 <= 0x9F)       // setcc m8: write only
            {
                d->action = kSkipWrite; d->eight = true; pos += 2; break;
            }
            if (op2 >= 0x40 && op2 <= 0x4F)       // cmovcc r32, m
            {
                d->action = kCmov; d->sub = static_cast<uint8_t>(op2 & 0x0F);
                pos += 2; break;
            }
            bool hasImm = false;
            const int cls = ClassifySse(op2, false, &hasImm);
            if (cls == 1)      { d->action = kZeroXmm;   pos += 2; d->imm8 = hasImm; }
            else if (cls == 2) { d->action = kSkipWrite; pos += 2; d->imm8 = hasImm; }
            else if (cls == 3) { d->action = kZero32;    pos += 2; }
            else if (cls == 4) { d->action = kComiXmm;   pos += 2; }
            else return false;
            break;
        }
        }
        break;
    }

    // x87 escapes. The reg field of the modrm selects the operation; only the
    // memory forms reach here at all, because a fault needs a memory operand.
    case 0xD8: case 0xDA: case 0xDC: case 0xDE:
    {
        switch ((p[pos + 1] >> 3) & 7)
        {
        case 0: case 4: d->action = kX87Skip;  break;   // fadd / fsub
        case 1: case 6: case 7: d->action = kX87Zero; break; // fmul/fdiv/fdivr
        case 5: d->action = kX87Neg;  break;            // fsubr
        case 2: d->action = kX87Com;  break;            // fcom
        case 3: d->action = kX87ComP; break;            // fcomp
        default: return false;
        }
        pos += 1;
        break;
    }
    case 0xD9: case 0xDD:
    {
        switch ((p[pos + 1] >> 3) & 7)
        {
        case 0: d->action = kX87Push0;   break;         // fld m32 / m64
        case 1: d->action = kX87Pop;     break;         // fisttp m64 (DD)
        case 2: d->action = kSkipWrite;  break;         // fst
        case 3: d->action = kX87Pop;     break;         // fstp
        case 7: d->action = kSkipWrite;  break;         // fnstcw / fnstsw
        // D9 /5 fldcw m16 - 489 sites, every float-to-int conversion the
        // compiler emitted. Leaving the control word alone is the only
        // completion available and matches "the memory held nothing".
        case 5:
            if (op != 0xD9) return false;               // DD /5 is not encoded
            d->action = kSkipWrite;
            break;
        default: return false;                          // fldenv/frstor/fnsave
        }
        pos += 1;
        break;
    }
    case 0xDB: case 0xDF:
    {
        switch ((p[pos + 1] >> 3) & 7)
        {
        case 0: case 5: d->action = kX87Push0; break;   // fild / fld m80
        case 1: case 3: case 7: d->action = kX87Pop; break; // fisttp/fistp/fstp
        case 2: d->action = kSkipWrite; break;          // fist
        default: return false;                          // fbld / fbstp
        }
        pos += 1;
        break;
    }

    default:
        return false;
    }

    const uint8_t modrm = p[pos];
    uint8_t reg = (modrm >> 3) & 7;
    int memLen = 0;
    if (!ParseMemOperand(p, pos, &memLen))
        return false;

    // Register-write validation. A destination register is only rejected
    // when emulating it would corrupt state we cannot reproduce:
    //   - 32-bit: esp (reg 4) is never a data destination; loading into it
    //     means the fault is real. ebp (reg 5) IS allowed: an instruction
    //     like `mov ebp,[m]` only occurs when the function uses ebp as a
    //     general register (a frame-pointer function would target eax/ecx),
    //     so writing ebp here is the true intent and safe.
    //   - 8-bit: only al/cl/dl/bl (reg 0-3) are reachable as low bytes of a
    //     GPR this emulator models. ah/ch/dh/bh (reg 4-7) would need the
    //     high byte, which Gpr()+&=~0xFF cannot touch, so reject them.
    const bool writesReg =
        d->action == kZero32 || d->action == kZero8 || d->action == kAndZero ||
        d->action == kXadd || d->action == kOr8FF || d->action == kCmov;
    // Actions whose reg field is a GPR operand rather than an opcode
    // extension. For the group opcodes (80-83, C0-D3, F6/F7, FE/FF, 8F,
    // 0F BA) reg selects the operation and must never be read as a register.
    const bool regIsGpr =
        writesReg || d->action == kCmpRm0 || d->action == kCmpNegR ||
        d->action == kTest0 || d->action == kAdd0 || d->action == kOr32FF ||
        d->action == kMAdd || d->action == kMOr || d->action == kMAnd ||
        d->action == kMSub || d->action == kMXor || d->action == kCmpxchg;
    if (d->eight && regIsGpr && reg > 3)
    {
        // ah/ch/dh/bh: the same four GPRs, one byte higher. 220 sites are
        // `and ah, byte ptr [m]` alone, and rejecting them was a limit of
        // Gpr(), not a property of the fault.
        d->high = true;
        reg = static_cast<uint8_t>(reg - 4);
    }
    else if (writesReg && !d->eight && reg == 4)
    {
        return false;              // mov esp, m: the fault is real
    }

    d->reg = reg;
    d->len = memLen;
    if (d->imm8)
        d->len += 1;                // SSE pshuf/cmp/shuf: trailing imm8
    d->len += d->immBytes;          // C7/C6/F7 test/C1 shift/69/6B/0F BA

    if (d->action == kAluImm)
    {
        // reg field is the ALU opcode (0=add,1=or,2=adc,3=sbb,4=and,5=sub,
        // 6=xor,7=cmp); the immediate follows the memory operand.
        //
        // d->sub says whether the immediate is a sign-extended imm8 (0x83)
        // and d->len comes from immBytes, like every other immediate: a
        // wrong length resumes in the middle of the next instruction.
        d->aluop = reg;
        const uint8_t* ip = p + memLen;
        if (d->immBytes == 1)
            d->imm = d->sub ? static_cast<uint32_t>(static_cast<int8_t>(*ip))
                            : *ip;
        else if (d->immBytes == 2)
            d->imm = *reinterpret_cast<const uint16_t*>(ip);
        else
            d->imm = *reinterpret_cast<const uint32_t*>(ip);
    }

    return d->len > 0;
}

DWORD* Gpr(PCONTEXT ctx, int reg)
{
    switch (reg)
    {
    case 0: return &ctx->Eax;
    case 1: return &ctx->Ecx;
    case 2: return &ctx->Edx;
    case 3: return &ctx->Ebx;
    case 6: return &ctx->Esi;
    case 7: return &ctx->Edi;
    default: return nullptr;
    }
}

uint8_t Parity(uint8_t v)
{
    uint8_t p = 1;
    while (v) { p ^= (v & 1); v >>= 1; }
    return p;                        // 1 = even parity
}

// EFLAGS: CF=0 PF=2 AF=4 ZF=6 SF=7 OF=11
// width: 8, 16 or 32 (operand width for ZF/SF masking).
void SetFlags(PCONTEXT ctx, uint32_t result, bool cf, bool of, int width)
{
    DWORD& fl = ctx->EFlags;
    fl &= ~((1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 11));
    if (cf) fl |= (1u << 0);
    if (of) fl |= (1u << 11);
    const uint32_t mask = width == 8 ? 0xFFu : width == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    const uint32_t sign = width == 8 ? 0x80u : width == 16 ? 0x8000u : 0x80000000u;
    const uint32_t v = result & mask;
    if (v == 0) fl |= (1u << 6);                                    // ZF
    if (v & sign) fl |= (1u << 7);                                  // SF
    if (Parity(static_cast<uint8_t>(v))) fl |= (1u << 2);           // PF
}

// Like SetFlags but preserves CF (inc/dec semantics).
void SetFlagsKeepCf(PCONTEXT ctx, uint32_t result, bool of, int width)
{
    const DWORD cf = ctx->EFlags & (1u << 0);
    SetFlags(ctx, result, false, of, width);
    ctx->EFlags = (ctx->EFlags & ~(1u << 0)) | cf;
}

// cmovcc condition, from the low nibble of the 0F 4x opcode.
bool CondHolds(DWORD fl, uint8_t cc)
{
    const bool cf = (fl & 0x001u) != 0, pf = (fl & 0x004u) != 0;
    const bool zf = (fl & 0x040u) != 0, sf = (fl & 0x080u) != 0;
    const bool of = (fl & 0x800u) != 0;
    bool t = false;
    switch (cc >> 1)
    {
    case 0: t = of;                 break;   // o
    case 1: t = cf;                 break;   // b
    case 2: t = zf;                 break;   // e
    case 3: t = cf || zf;           break;   // be
    case 4: t = sf;                 break;   // s
    case 5: t = pf;                 break;   // p
    case 6: t = (sf != of);         break;   // l
    case 7: t = zf || (sf != of);   break;   // le
    }
    return (cc & 1) ? !t : t;
}

LONG WINAPI CrashGuardHandler(PEXCEPTION_POINTERS ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;

    // STATUS_GUARD_PAGE_VIOLATION (0x80000001): a wild write that happened
    // to land on a PAGE_GUARD page. It carries the same ExceptionInformation
    // layout as an AV (op, target) and is emulable exactly like one, e.g.
    // `movaps [esp+eax*8+0xC0],xmm0` with a wild index hitting a guard page.
    // Exception: faults whose target is close to the current stack pointer
    // are treated as legitimate stack growth and passed to the system
    // handler untouched (the game must be able to grow its threads' stacks).
    if (code == STATUS_GUARD_PAGE_VIOLATION)
    {
        const EXCEPTION_RECORD* gr = ep->ExceptionRecord;
        const PCONTEXT gc = ep->ContextRecord;
        const bool stackish =
            gr->NumberParameters >= 2 &&
            gr->ExceptionInformation[1] >= gc->Esp - 0x100000u &&
            gr->ExceptionInformation[1] <  gc->Esp + 0x10000u;
        if (stackish)
        {
            LogFault(ep);                          // capture, then let it happen
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // fall through: decode & emulate like an access violation
    }
    else if (code != EXCEPTION_ACCESS_VIOLATION)
    {
        // Only access violations are emulable. Log every other fatal-looking
        // exception (stack overflow 0xC00000FD, ...) so a crash is never
        // silent, then pass it on. Informational exceptions fire constantly
        // and are skipped, as are C++ exceptions, which the code that throws
        // them normally catches.
        const bool benign =
            code == 0x80000003 ||            // breakpoint
            code == 0x40010006 ||            // DBG_PRINTEXCEPTION_C
            code == 0x40010007 ||            // DBG_PRINTEXCEPTION_WIDE_C
            code == 0x406D1388 ||            // SetThreadName
            code == 0x4000001E ||            // MS_VC_EXCEPTION
            code == 0x80000004 ||            // single step
            code == 0xE06D7363;              // C++ throw
        if (!benign)
            LogFault(ep);                    // guard page, stack overflow, etc.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uint32_t eip = ep->ContextRecord->Eip;
    if (eip < kTextStart || eip >= kTextEnd)
    {
        // Our own reads of game memory run under __try and handle their
        // faults themselves; anything else outside game code is fatal.
        if (!InOwnModule(eip))
            LogFault(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    Decoded d;
    if (!Decode(reinterpret_cast<const uint8_t*>(eip), &d))
    {
        LogFault(ep);                              // unclassifiable: capture it
        return EXCEPTION_CONTINUE_SEARCH;          // not an emulable instruction
    }

    PCONTEXT ctx = ep->ContextRecord;
    DWORD* r = Gpr(ctx, d.reg);
    const DWORD v = r ? *r : 0;
    const int width = d.eight ? 8 : (d.sixteen ? 16 : 32);
    const uint32_t mask = width == 8 ? 0xFFu : width == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    const uint32_t sign = width == 8 ? 0x80u : width == 16 ? 0x8000u : 0x80000000u;
    // Operand-width view of the register. d.high is ah/ch/dh/bh: the same GPR,
    // one byte up.
    const uint32_t vw = d.high ? ((v >> 8) & 0xFFu) : (v & mask);
    const uint32_t lowMask = d.high ? 0xFF00u : 0xFFu;

    switch (d.action)
    {
    case kZero32:
        if (r)
        {
            if (d.sixteen) *r &= ~0xFFFFu;    // 16-bit load: preserve upper
            else           *r = 0;
        }
        break;
    case kZero8:
        if (r) *r &= ~lowMask;
        break;
    case kZeroXmm:
        if (d.reg <= 7)
        {
            M128A* xmm = reinterpret_cast<M128A*>(
                reinterpret_cast<BYTE*>(ctx) + kXmmOffset);
            xmm[d.reg].Low = 0;
            xmm[d.reg].High = 0;
        }
        break;
    case kComiXmm:
    {
        // comiss/ucomiss xmm, m -> the memory operand read as 0.0. OF, SF and
        // AF are always cleared; ZF/PF/CF carry the result, with the unordered
        // case (the xmm side is NaN) setting all three.
        if (d.reg > 7)
            return EXCEPTION_CONTINUE_SEARCH;
        const M128A* xmm = reinterpret_cast<const M128A*>(
            reinterpret_cast<const BYTE*>(ctx) + kXmmOffset);
        bool unordered, above, below;
        if (d.sixteen)              // comisd/ucomisd: 66 prefix, double
        {
            double a;
            memcpy(&a, &xmm[d.reg].Low, sizeof(a));
            unordered = !(a == a);
            above = !unordered && a > 0.0;
            below = !unordered && a < 0.0;
        }
        else
        {
            float a;
            memcpy(&a, &xmm[d.reg].Low, sizeof(a));
            unordered = !(a == a);
            above = !unordered && a > 0.0f;
            below = !unordered && a < 0.0f;
        }
        DWORD& fl = ctx->EFlags;
        fl &= ~((1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) |
                (1u << 11));
        if (unordered)      fl |= (1u << 0) | (1u << 2) | (1u << 6);  // CF PF ZF
        else if (below)     fl |= (1u << 0);                          // CF
        else if (!above)    fl |= (1u << 6);                          // ZF
        break;
    }
    case kOr8FF:
        if (r) *r |= lowMask;
        SetFlags(ctx, vw | 0xFFu, false, false, 8);
        break;
    case kOr32FF:
        if (r)
        {
            if (d.sixteen) *r |= 0xFFFFu;
            else           *r = 0xFFFFFFFFu;
        }
        SetFlags(ctx, v | mask, false, false, width);
        break;
    case kAndZero:
        if (r)
        {
            if (d.eight)        *r &= ~lowMask;   // and r8, 0
            else if (d.sixteen) *r &= ~0xFFFFu;   // and r16, 0: preserve upper
            else                *r = 0;
        }
        SetFlags(ctx, 0, false, false, width);
        break;
    case kAdd0:                     // add/sub/xor r, 0: r unchanged
        SetFlags(ctx, vw, false, false, width);
        break;
    case kSkipWrite:
        break;
    case kX87Skip:
        break;
    case kX87Zero:
        FpuSt0Zero(ctx);
        break;
    case kX87Neg:
        FpuNegSt0(ctx);
        break;
    case kX87Com:
    case kX87ComP:
        FpuComZero(ctx);
        if (d.action == kX87ComP)
            FpuPop(ctx);
        break;
    case kX87Push0:
        FpuPush0(ctx);
        break;
    case kX87Pop:
        FpuPop(ctx);
        break;
    case kCmpRm0:                   // r - 0
        SetFlags(ctx, vw, false, false, width);
        break;
    case kCmpNegR:                  // 0 - r
        SetFlags(ctx, 0u - vw, vw != 0, vw == sign, width);
        break;
    case kTest0:
        SetFlags(ctx, 0, false, false, width);
        break;
    case kMAdd:                     // add m(0), r
    case kMOr:
    case kMXor:
        SetFlags(ctx, vw, false, false, width);
        break;
    case kMAnd:                     // and m(0), r -> 0
        SetFlags(ctx, 0, false, false, width);
        break;
    case kMSub:                     // sub m(0), r -> 0 - r
        SetFlags(ctx, 0u - vw, vw != 0, vw == sign, width);
        break;
    case kAluImm:                   // add/or/adc/sbb/and/sub/xor/cmp m(0), imm
    {
        // Memory operand is 0 (that's why the read faulted). Compute flags
        // for "0 op imm", skip the write back to the (invalid) memory addr.
        const uint32_t imm = d.imm & mask;
        const bool cf = (ctx->EFlags & (1u << 0)) != 0;
        uint32_t r2 = 0;
        bool of = false, carry = false;
        switch (d.aluop)
        {
        case 0: r2 = imm;                     break;   // add 0 + imm
        case 1: r2 = imm;                     break;   // or  0 | imm
        case 2: r2 = imm + (cf ? 1u : 0u); carry = (r2 < (cf ? 1u : 0u)); break; // adc
        case 3: r2 = 0u - imm - (cf ? 1u : 0u); carry = (0u < imm + (cf ? 1u : 0u)); break; // sbb
        case 4: r2 = 0;                       break;   // and 0 & imm
        case 5: r2 = 0u - imm; carry = (imm != 0); break; // sub 0 - imm
        case 6: r2 = imm;                     break;   // xor 0 ^ imm
        case 7: r2 = 0u - imm; carry = (imm != 0); break; // cmp 0 - imm
        }
        // Sign/overflow for the low operand width.
        const uint32_t rv = r2 & mask;
        const bool sf = (rv & sign) != 0;
        const bool zf = rv == 0;
        DWORD& fl = ctx->EFlags;
        fl &= ~((1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 11));
        if (carry) fl |= (1u << 0);
        if (of)    fl |= (1u << 11);
        if (zf)    fl |= (1u << 6);
        if (sf)    fl |= (1u << 7);
        if (Parity(static_cast<uint8_t>(rv))) fl |= (1u << 2);
        break;
    }
    case kXadd:                     // xadd m(0), r: r = old = 0, flags 0+r
        if (r)
        {
            if (d.eight)        *r &= ~lowMask;
            else if (d.sixteen) *r &= ~0xFFFFu;
            else                *r = 0;
        }
        SetFlags(ctx, vw, false, false, width);
        break;
    case kIncM:                     // inc m(0) -> 1
        SetFlagsKeepCf(ctx, 1, false, width);
        break;
    case kDecM:                     // dec m(0) -> -1
        SetFlagsKeepCf(ctx, mask, false, width);
        break;
    case kCmpxchg:                  // cmpxchg m(0), r vs accumulator
    {
        const uint32_t acc = ctx->Eax & mask;
        SetFlags(ctx, acc, acc != 0, false, width);     // acc - 0
        if (acc != 0)               // not equal: accumulator = memory (0)
        {
            if (d.eight)        ctx->Eax &= ~0xFFu;
            else if (d.sixteen) ctx->Eax &= ~0xFFFFu;
            else                ctx->Eax = 0;
        }
        break;
    }
    case kRepString:                // rep movs/stos/lods/... : complete, ECX=0
        ctx->Ecx = 0;               // caller sees the transfer as finished
        break;
    case kPopM:                     // pop m: write skipped, stack still pops
        ctx->Esp += d.sixteen ? 2u : 4u;
        break;
    case kZeroFlags:                // neg/shift/test/bt m(0): result 0
        SetFlags(ctx, 0, false, false, width);
        break;
    case kMulM:                     // mul/imul m(0): product 0, CF=OF=0
        if (d.eight) ctx->Eax &= ~0xFFFFu;    // AX = AL * 0
        else { ctx->Eax = 0; ctx->Edx = 0; }
        SetFlags(ctx, 0, false, false, width);
        break;
    case kDivM:                     // div/idiv m(0): quotient 0, remainder 0
        if (d.eight) ctx->Eax &= ~0xFFFFu;    // AL = quotient, AH = remainder
        else { ctx->Eax = 0; ctx->Edx = 0; }
        break;
    case kCmov:                     // cmovcc r, m(0)
        if (r && CondHolds(ctx->EFlags, d.sub))
        {
            if (d.sixteen) *r &= ~0xFFFFu;
            else           *r = 0;
        }
        break;
    case kCmpx8:                    // cmpxchg8b m(0) vs EDX:EAX
    {
        const bool eq = (ctx->Eax == 0 && ctx->Edx == 0);
        if (eq) ctx->EFlags |= (1u << 6);
        else  { ctx->EFlags &= ~(1u << 6); ctx->Eax = 0; ctx->Edx = 0; }
        break;
    }
    case kStrOne:                   // one movs/stos/lods/cmps/scas, no REP
    {
        const LONG step = d.eight ? 1 : (d.sixteen ? 2 : 4);
        const LONG delta = (ctx->EFlags & 0x400u) ? -step : step;
        switch (d.sub)
        {
        case 0xA4: case 0xA5:                       // movs: transfer skipped
            ctx->Esi += delta; ctx->Edi += delta;
            break;
        case 0xA6: case 0xA7:                       // cmps: 0 - 0
            ctx->Esi += delta; ctx->Edi += delta;
            SetFlags(ctx, 0, false, false, width);
            break;
        case 0xAA: case 0xAB:                       // stos: write skipped
            ctx->Edi += delta;
            break;
        case 0xAC: case 0xAD:                       // lods: accumulator = 0
            ctx->Esi += delta;
            if (d.eight)        ctx->Eax &= ~0xFFu;
            else if (d.sixteen) ctx->Eax &= ~0xFFFFu;
            else                ctx->Eax = 0;
            break;
        case 0xAE: case 0xAF:                       // scas: acc - 0
            ctx->Edi += delta;
            SetFlags(ctx, ctx->Eax & mask, false, false, width);
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
        }
        break;
    }
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    ctx->Eip += d.len;

    const LONG hits = InterlockedIncrement(&g_hitCount);
    if (hits == 1)
    {
        InterlockedExchange(&g_sanitizeArmed, 1);
        LOG_INFO("crash guard: first emulated fault at 0x%08lX", static_cast<unsigned long>(eip));
    }
    LogEmulatedFault(eip, ep->ExceptionRecord);
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

long EmulatedFaultCount()
{
    return g_hitCount;
}

void InstallCrashGuard()
{
    AddVectoredExceptionHandler(1, &CrashGuardHandler);
}
