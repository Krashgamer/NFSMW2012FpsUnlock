#include "player.h"
#include "../patch.h"

#include <cstring>

namespace player
{
namespace
{
constexpr uintptr_t kMergeFn   = 0xA9A370;   // sub_A9A370, the registry merge
constexpr uintptr_t kMergeCall = 0x4B1F3E;   // its call site in sub_4B1EE0
constexpr size_t    kStride    = 32;         // registry slot / queue record size
constexpr uint8_t   kTypePlayer = 2;         // slot byte +26

float g_pos[3] = { 0.0f, 0.0f, 0.0f };
volatile bool g_posOk = false;
const void* volatile g_car = nullptr;        // latest car object at that position
const void* g_frameCar = nullptr;
} // namespace

uintptr_t g_playerMergeFn = kMergeFn;

// Runs right after the original merge. The queue (record xyz at +0, slot
// index u16 at +16) names every slot written this tick.
extern "C" void __cdecl PlayerNoteMerge(uint32_t* pool, uint32_t* queue)
{
    __try
    {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(pool[1]);
        const uint8_t* rec = reinterpret_cast<const uint8_t*>(queue[0]);
        const int n = static_cast<int>(queue[2]);
        if (!base || !rec || n <= 0)
            return;
        for (int i = 0; i < n; ++i)
        {
            const uint32_t idx = *reinterpret_cast<const uint16_t*>(rec + kStride * i + 16);
            if (base[kStride * idx + 26] == kTypePlayer)
            {
                memcpy(g_pos, rec + kStride * i, sizeof(g_pos));
                g_posOk = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// __thiscall(this = registry, one stack argument = queue), retn 4.
extern "C" __declspec(naked) void PlayerMergeThunk()
{
    __asm
    {
        push ebp
        mov  ebp, esp
        sub  esp, 4
        mov  [ebp-4], ecx               // the registry

        push dword ptr [ebp+8]          // the queue
        mov  ecx, [ebp-4]
        mov  eax, g_playerMergeFn
        call eax                        // original merge, cleans its argument

        push dword ptr [ebp+8]
        push dword ptr [ebp-4]
        call PlayerNoteMerge
        add  esp, 8

        mov  esp, ebp
        pop  ebp
        ret  4
    }
}

bool InstallRegistryHook()
{
    return patch::RedirectCall(kMergeCall, kMergeFn, reinterpret_cast<const void*>(&PlayerMergeThunk));
}

void NoteCarTransform(const void* obj)
{
    if (!g_posOk)
        return;
    const float* m = reinterpret_cast<const float*>(static_cast<const char*>(obj) + 224);
    // The registry position sits ~4 m from the body origin.
    const float dx = m[12] - g_pos[0], dy = m[13] - g_pos[1], dz = m[14] - g_pos[2];
    if (dx >= -5.0f && dx <= 5.0f && dy >= -5.0f && dy <= 5.0f && dz >= -5.0f && dz <= 5.0f)
        g_car = obj;
}

void BeginFrame()
{
    g_frameCar = g_car;
}

const void* CarObject()
{
    return g_frameCar;
}
} // namespace player
