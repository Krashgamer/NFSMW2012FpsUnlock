#include "streamsync.h"
#include "gamestate.h"
#include "pacer.h"
#include "patch.h"

#include <cstdint>

namespace
{
constexpr uintptr_t kStreamCall   = 0x476FD6;   // call sub_47E1E0 (ecx = game object + 0x3400)
constexpr uintptr_t kStreamUpdate = 0x47E1E0;

extern "C" bool __cdecl StreamSyncShouldStep()
{
    return CurrentLoadingScreen() != LoadingScreen::None || TickDueBeforeNextFrame();
}
} // namespace

bool InstallStreamSync()
{
    if (patch::CallTarget(kStreamCall) != kStreamUpdate)
        return false;
    uint8_t* mem = patch::AllocCode(32);
    if (!mem)
        return false;
    // push ecx / call decide / pop ecx / test al,al / jz skip /
    // jmp sub_47E1E0 (returns to the call site) / skip: ret
    patch::Stub s{ mem };
    s.B(0x51);
    s.Call(reinterpret_cast<const void*>(&StreamSyncShouldStep));
    s.B(0x59);
    s.B(0x84); s.B(0xC0);
    s.B(0x74); s.B(0x05);
    s.Jmp(kStreamUpdate);
    s.B(0xC3);
    patch::Branch(kStreamCall, 0xE8, mem);
    return true;
}
