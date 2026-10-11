#include "view.h"
#include "xform.h"

#include <cstdint>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace view
{
namespace
{
constexpr uintptr_t kRenderMgrPtr = 0x10EDAF4;   // block = *(*(0x10EDAF4) + 4)
constexpr size_t    kDw = 1024 / 4;

uint32_t g_prev[kDw];
uint32_t g_cur[kDw];
uint32_t g_out[kDw];
bool g_have = false;
bool g_wrote = false;      // our blend is in memory and awaits restore
bool g_valid = false;      // g_out still describes what we last wrote

uint8_t* Block()
{
    __try
    {
        const uintptr_t p = *reinterpret_cast<const uintptr_t*>(kRenderMgrPtr);
        if (p < 0x10000)
            return nullptr;
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(p + 4);
        if (b < 0x10000)
            return nullptr;
        return reinterpret_cast<uint8_t*>(b);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}
} // namespace

void Apply(float alpha)
{
    g_wrote = false;
    uint8_t* at = Block();
    if (!at)
        return;
    uint32_t v[kDw];
    __try
    {
        memcpy(v, at, sizeof(v));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_have = false;
        return;
    }
    if (!g_have)
    {
        memcpy(g_cur, v, sizeof(v));
        memcpy(g_prev, v, sizeof(v));
        g_have = true;
        g_valid = false;
        return;
    }
    // Memory that still holds exactly our last write is not a new tick.
    const bool ours = g_valid && memcmp(v, g_out, sizeof(v)) == 0;
    if (!ours && memcmp(v, g_cur, sizeof(v)) != 0)
    {
        memcpy(g_prev, g_cur, sizeof(v));
        memcpy(g_cur, v, sizeof(v));
    }
    blend::LerpFields(g_out, g_prev, g_cur, kDw, alpha);
    __try
    {
        memcpy(at, g_out, sizeof(g_out));
        g_wrote = true;
        g_valid = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_valid = false;
    }
}

// Non-destructive: only puts truth back while memory still holds our blend.
void Restore()
{
    if (!g_wrote)
        return;
    g_wrote = false;
    uint8_t* at = Block();
    if (!at)
        return;
    __try
    {
        if (memcmp(at, g_out, sizeof(g_out)) == 0)
            memcpy(at, g_cur, sizeof(g_cur));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_valid = false;
}
} // namespace view
