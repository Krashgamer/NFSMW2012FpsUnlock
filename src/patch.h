#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>

// Code patching helpers. All game addresses are absolute: NFS13.exe v1.5 is
// not relocated (image base 0x400000), this DLL is.
namespace patch
{
inline void Write(uintptr_t address, const void* data, size_t size)
{
    DWORD oldProtect{};
    VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(reinterpret_cast<void*>(address), data, size);
    VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
}

inline bool Matches(uintptr_t address, const void* data, size_t size)
{
    return memcmp(reinterpret_cast<const void*>(address), data, size) == 0;
}

inline int32_t Rel32(uintptr_t from, uintptr_t to)
{
    return static_cast<int32_t>(to - from - 5);
}

// Writes `E8/E9 rel32` at `site`.
inline void Branch(uintptr_t site, uint8_t opcode, const void* target)
{
    uint8_t b[5] = { opcode };
    const int32_t rel = Rel32(site, reinterpret_cast<uintptr_t>(target));
    memcpy(b + 1, &rel, 4);
    Write(site, b, sizeof(b));
}

inline void Jump(uintptr_t site, const void* target) { Branch(site, 0xE9, target); }

// The current target of an `E8 rel32` at `site`, or 0 if `site` is not one.
inline uintptr_t CallTarget(uintptr_t site)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
    if (p[0] != 0xE8)
        return 0;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return site + 5 + rel;
}

// Retargets the `E8 rel32` at `site` to `hook`, but only while it still calls
// `expected`.
inline bool RedirectCall(uintptr_t site, uintptr_t expected, const void* hook)
{
    if (CallTarget(site) != expected)
        return false;
    Branch(site, 0xE8, hook);
    return true;
}

inline uint8_t* AllocCode(size_t n)
{
    return static_cast<uint8_t*>(
        VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
}

// Tiny x86 emitter for the hand-written stubs.
struct Stub
{
    uint8_t* p;
    size_t   o = 0;

    uintptr_t Here() const { return reinterpret_cast<uintptr_t>(p) + o; }
    void B(uint8_t v) { p[o++] = v; }
    void D32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            B(static_cast<uint8_t>(v >> (8 * i)));
    }
    void Bytes(const uint8_t* s, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            B(s[i]);
    }
    void Call(const void* target)            // E8 rel32
    {
        const uintptr_t at = Here();
        B(0xE8);
        D32(static_cast<uint32_t>(Rel32(at, reinterpret_cast<uintptr_t>(target))));
    }
    void Jmp(uintptr_t target)               // E9 rel32
    {
        const uintptr_t at = Here();
        B(0xE9);
        D32(static_cast<uint32_t>(Rel32(at, target)));
    }
    void LockInc(volatile LONG* counter)     // lock inc dword ptr [counter]
    {
        B(0xF0); B(0xFF); B(0x05); D32(reinterpret_cast<uint32_t>(counter));
    }
    void LockDec(volatile LONG* counter)     // lock dec dword ptr [counter]
    {
        B(0xF0); B(0xFF); B(0x0D); D32(reinterpret_cast<uint32_t>(counter));
    }
};

// Detours `site` to `stub`: a jmp over the first `stolen` bytes, padded with
// nops. The stub must already contain those bytes (or their equivalent).
inline void Detour(uintptr_t site, const void* stub, size_t stolen)
{
    uint8_t b[16];
    memset(b, 0x90, sizeof(b));
    b[0] = 0xE9;
    const int32_t rel = Rel32(site, reinterpret_cast<uintptr_t>(stub));
    memcpy(b + 1, &rel, 4);
    Write(site, b, stolen);
}
} // namespace patch
