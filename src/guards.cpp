#include "guards.h"
#include "patch.h"

// ---------------------------------------------------------------------------
// Descriptor slot-index bounds guards
// ---------------------------------------------------------------------------
// sub_A5E080 dedupes the resource-slot id list of a render descriptor through
// a 64-byte "seen" bitmap on its stack:
//
//     a5e0b0  mov   al, [esi+edx+105h]    ; slot id, 0..63 by contract
//     a5e0b7  movzx ecx, al
//     a5e0bf  lea   ecx, [esp+ecx+8]
//     a5e0cc  mov   byte ptr [ecx], 1
//
// The return address sits right after the bitmap, so an id of 67 writes a 1
// into its high byte (0x00A62833 -> 0x01A62833). sub_A5D5F0 is the same
// routine for the 195-slot descriptor. The ids are stored by sub_A62820 /
// sub_A621A0, which also index a fixed slot array with them. Under uncapped
// frame rates a render worker can read the index out of a descriptor the game
// thread is rebuilding, so it can be anything.
//
// Every bound is a property of the structure (bitmap length, slot array
// length). A legal id takes the original path instruction for instruction:
//
//   0xA5E0B0  sub_A5E080 loop  - drop an id past the end of the 64-byte bitmap
//   0xA5D620  sub_A5D5F0 loop  - drop an id past the end of the 195-byte bitmap
//   0xA62820  entry            - an out-of-range slot index binds into a
//   0xA621A0  entry              16-byte aligned scratch sink instead of
//                                writing past the slot array
// ---------------------------------------------------------------------------
namespace
{
volatile LONG g_droppedIds = 0;
volatile LONG g_sunkBinds = 0;
volatile LONG g_lastBadId = 0;

// Callers write up to 16 bytes through the returned pointer (`movaps` at
// 0x54797B), so it must be 16-byte aligned. Shared by all worker threads on
// purpose - nothing ever reads it back.
__declspec(align(16)) uint8_t g_slotSink[64];

// `site` is the 7-byte `mov al, [esi+edx+disp32]` at the top of the loop,
// `maxId` the last index the bitmap covers, `contBad` the loop-continue label.
// A good id rejoins at site+7 (`movzx ecx, al`).
bool InstallDedupeGuard(uintptr_t site, const uint8_t (&load)[7], uint8_t maxId,
                        uintptr_t contBad)
{
    if (!patch::Matches(site, load, 7))
        return false;
    uint8_t* mem = patch::AllocCode(64);
    if (!mem)
        return false;
    patch::Stub s{ mem };
    s.Bytes(load, 7);                                            // mov al, [...]
    s.B(0x3C); s.B(maxId);                                       // cmp al, maxId
    s.B(0x76); s.B(0x11);                                        // jbe ok
    s.B(0xA2); s.D32(reinterpret_cast<uint32_t>(&g_lastBadId));  // mov [lastBadId], al
    s.LockInc(&g_droppedIds);
    s.Jmp(contBad);                                              // drop it, continue
    s.Jmp(site + 7);                                             // ok:
    patch::Detour(site, mem, 7);
    return true;
}

// `site` is the function entry, `stolen` its leading bytes (>= 5, no relative
// branches). Both targets are __thiscall with one stack argument and retn 4.
bool InstallSlotGuard(uintptr_t site, const uint8_t* stolen, size_t nStolen, uint32_t maxIdx)
{
    if (!patch::Matches(site, stolen, nStolen))
        return false;
    uint8_t* mem = patch::AllocCode(64);
    if (!mem)
        return false;
    patch::Stub s{ mem };
    if (maxIdx < 0x80)
    {
        s.B(0x83); s.B(0x7C); s.B(0x24); s.B(0x04); s.B(static_cast<uint8_t>(maxIdx));
    }
    else
    {
        s.B(0x81); s.B(0x7C); s.B(0x24); s.B(0x04); s.D32(maxIdx);   // cmp [esp+4], maxIdx
    }
    s.B(0x76); s.B(0x0F);                                        // jbe pass
    s.LockInc(&g_sunkBinds);
    s.B(0xB8); s.D32(reinterpret_cast<uint32_t>(g_slotSink));    // mov eax, sink
    s.B(0xC2); s.B(0x04); s.B(0x00);                             // retn 4
    s.Bytes(stolen, nStolen);                                    // pass:
    s.Jmp(site + nStolen);
    patch::Detour(site, mem, nStolen);
    return true;
}
} // namespace

int InstallSlotBoundsGuards()
{
    static const uint8_t kLoad5E[7] = { 0x8A, 0x84, 0x16, 0x05, 0x01, 0x00, 0x00 };
    static const uint8_t kLoad5D[7] = { 0x8A, 0x84, 0x16, 0x11, 0x03, 0x00, 0x00 };
    static const uint8_t kEnter62820[5] = { 0x53, 0x56, 0x57, 0x8B, 0xF9 };
    static const uint8_t kEnter621A0[11] =
        { 0x53, 0x55, 0x8B, 0xE9, 0x80, 0xBD, 0x10, 0x03, 0x00, 0x00, 0xFF };

    int n = 0;
    n += InstallDedupeGuard(0xA5E0B0, kLoad5E, 0x3F, 0xA5E0D0) ? 1 : 0;
    n += InstallDedupeGuard(0xA5D620, kLoad5D, 0xC2, 0xA5D640) ? 1 : 0;
    n += InstallSlotGuard(0xA62820, kEnter62820, sizeof(kEnter62820), 0x3F) ? 1 : 0;
    n += InstallSlotGuard(0xA621A0, kEnter621A0, sizeof(kEnter621A0), 0xC2) ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------------------
// Command-stream replay guards
// ---------------------------------------------------------------------------
// The crash guard's contract is that a faulting read in game code reads as 0.
// When the bucket builder walks a descriptor that is being rebuilt, those
// zeros and near-zeros get recorded into the render command buffer as
// resource pointers, and the replay then hands them to D3D11 - where an
// invalid view pointer crashes inside d3d11.dll (READ 0x4 at
// d3d11.dll+0x16DB28 via PSSetShaderResources).
//
// The replay is the last place those arguments are still ours. Every object
// d3d11.dll hands out has its vtable inside d3d11.dll, so an entry that is not
// NULL and whose vtable is not in d3d11.dll cannot be a view, buffer or
// sampler - binding NULL in its place is the legal way to say so.
//
// The sanitiser runs per replayed bind command, so it stays off (one compare)
// until the crash guard has emulated its first fault: before the first torn
// read there is nothing in the buffer it could catch.
//
// The 24 bind handlers share one exact 29-byte prologue that reads StartSlot
// and Count, aligns the cursor and hands (StartSlot, Count, array) to a context
// vtable slot; they are found by scanning .text for it.
// ---------------------------------------------------------------------------
volatile LONG g_sanitizeArmed = 0;

namespace
{
volatile LONG g_badOpcodes = 0;
uintptr_t g_d3d11Lo = 0;
uintptr_t g_d3d11Hi = 0;

bool D3D11Range()
{
    if (g_d3d11Lo)
        return true;
    HMODULE m = GetModuleHandleA("d3d11.dll");
    if (!m)
        return false;
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_d3d11Hi = base + nt->OptionalHeader.SizeOfImage;
    g_d3d11Lo = base;
    return true;
}

// Only ever writes NULL, and only over a pointer that cannot be a D3D11
// object. The cap is on our own work: a longer array keeps its tail as is.
void __cdecl SanitizeBinds(void** arr, unsigned count)
{
    if (!arr || count == 0 || !D3D11Range())
        return;
    if (count > 256)
        count = 256;
    __try
    {
        for (unsigned i = 0; i < count; ++i)
        {
            const uintptr_t p = reinterpret_cast<uintptr_t>(arr[i]);
            if (p == 0)
                continue;                       // legal: unbinds the slot
            if (p < 0x10000 || (p & 3) != 0)
            {
                arr[i] = nullptr;
                continue;
            }
            const uintptr_t vt = *reinterpret_cast<uintptr_t*>(p);
            if (vt < g_d3d11Lo || vt >= g_d3d11Hi)
                arr[i] = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

constexpr uint8_t kReplayPrologue[29] = {
    0x8B, 0x01, 0x8B, 0x50, 0x08, 0x53, 0x56, 0x57,
    0x8B, 0x3A, 0x83, 0xC2, 0x04, 0x89, 0x50, 0x08,
    0x8B, 0x01, 0x8B, 0x50, 0x08, 0x8B, 0x32, 0x83,
    0xC2, 0x04, 0x89, 0x50, 0x08
};
// Then: mov edx,[ecx] / mov eax,[edx+8] / add eax,0Fh / and eax,0FFFFFFF0h
constexpr uint8_t kReplayAlign[11] = {
    0x8B, 0x11, 0x8B, 0x42, 0x08, 0x83, 0xC0, 0x0F, 0x83, 0xE0, 0xF0
};
// The seven bytes the guard takes over, with eax = the array and esi = the
// count. Two orderings exist (18 sites push the array first, 6 the count
// first); they are the same instructions either way.
constexpr uint8_t kReplayStealA[7] = { 0x50, 0x8D, 0x1C, 0xB0, 0x89, 0x5A, 0x08 };
constexpr uint8_t kReplayStealB[7] = { 0x8D, 0x1C, 0xB0, 0x56, 0x89, 0x5A, 0x08 };
constexpr size_t kStealLen = 7;

bool InstallReplaySanitize(uintptr_t site)
{
    const uint8_t* steal = patch::Matches(site, kReplayStealA, kStealLen) ? kReplayStealA
                         : patch::Matches(site, kReplayStealB, kStealLen) ? kReplayStealB
                         : nullptr;
    if (!steal)
        return false;
    uint8_t* mem = patch::AllocCode(64);
    if (!mem)
        return false;
    patch::Stub s{ mem };
    s.B(0x83); s.B(0x3D); s.D32(reinterpret_cast<uint32_t>(&g_sanitizeArmed)); s.B(0x00);
    s.B(0x74); s.B(0x0C);                    // jz over
    s.B(0x60);                               // pushad
    s.B(0x56);                               // push esi   (count)
    s.B(0x50);                               // push eax   (array)
    s.Call(reinterpret_cast<const void*>(&SanitizeBinds));
    s.B(0x83); s.B(0xC4); s.B(0x08);         // add esp, 8
    s.B(0x61);                               // popad
    s.Bytes(steal, kStealLen);               // over:
    s.Jmp(site + kStealLen);
    patch::Detour(site, mem, kStealLen);
    return true;
}

// The replay dispatcher indexes a 70-entry handler table with the opcode it
// reads from the buffer. The bound is counted off the table and re-verified.
constexpr uintptr_t kReplayDispatch = 0xD9ABE3;
constexpr uintptr_t kReplayResume   = 0xD9ABEE;   // mov ecx, esi
constexpr uintptr_t kReplayDone     = 0xD9ABF4;   // loop exit
constexpr uintptr_t kReplayTable    = 0x10E6C50;
constexpr uint32_t  kMaxOpcode      = 69;

bool InstallReplayOpcodeGuard()
{
    const uint8_t stolen[11] = {
        0x85, 0xD2,                                     // test edx, edx
        0x74, 0x0D,                                     // jz   loc_D9ABF4
        0x8B, 0x04, 0x95, 0x50, 0x6C, 0x0E, 0x01        // mov eax, table[edx*4]
    };
    if (!patch::Matches(kReplayDispatch, stolen, sizeof(stolen)))
        return false;
    const uint32_t* tbl = reinterpret_cast<const uint32_t*>(kReplayTable);
    if (tbl[kMaxOpcode] == 0 || tbl[kMaxOpcode + 1] != 0)
        return false;
    uint8_t* mem = patch::AllocCode(64);
    if (!mem)
        return false;
    patch::Stub s{ mem };
    s.B(0x85); s.B(0xD2);                                // test edx, edx
    s.B(0x74); s.B(0x18);                                // jz   end
    s.B(0x83); s.B(0xFA); s.B(static_cast<uint8_t>(kMaxOpcode));
    s.B(0x77); s.B(0x0C);                                // ja   bad
    s.B(0x8B); s.B(0x04); s.B(0x95); s.D32(kReplayTable);
    s.Jmp(kReplayResume);
    s.LockInc(&g_badOpcodes);                            // bad:
    s.Jmp(kReplayDone);                                  // end:
    patch::Detour(kReplayDispatch, mem, sizeof(stolen));
    return true;
}
} // namespace

int InstallReplayGuards()
{
    int n = InstallReplayOpcodeGuard() ? 1 : 0;

    const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (memcmp(sec->Name, ".text", 5) != 0)
            continue;
        const uintptr_t start = reinterpret_cast<uintptr_t>(base) + sec->VirtualAddress;
        const uintptr_t end = start + sec->Misc.VirtualSize;
        const size_t head = sizeof(kReplayPrologue) + sizeof(kReplayAlign);
        const size_t span = head + kStealLen;
        for (uintptr_t p = start; p + span <= end; ++p)
        {
            if (*reinterpret_cast<const uint16_t*>(p) != 0x018B)
                continue;                       // cheap reject: mov eax,[ecx]
            if (!patch::Matches(p, kReplayPrologue, sizeof(kReplayPrologue)) ||
                !patch::Matches(p + sizeof(kReplayPrologue), kReplayAlign, sizeof(kReplayAlign)))
                continue;
            if (InstallReplaySanitize(p + head))
            {
                ++n;
                p += span - 1;
            }
        }
    }
    return n;
}
