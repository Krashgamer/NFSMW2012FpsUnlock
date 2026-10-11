#include "gamestate.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace
{
// The game logic module (stored by its init, sub_6AAE60) and its
// GameModeManager.
constexpr uintptr_t kGameLogic   = 0x1177868;
constexpr uint32_t  kModeManager = 0x2F3FC;

// GameModeManager fields, named after the properties the game registers for
// them (sub_70A120).
constexpr uint32_t kCurrentEvent = 0x2CE0;  // WorldLoaded = this != 0 && kWorldReady
constexpr uint32_t kWorldReady   = 0x2E77;
constexpr uint32_t kHasNextEvent = 0x2E7E;  // HasNextEvent
constexpr uint32_t kIntroLoaded  = 0x2E82;  // HasIntroLoadCompleted

template <typename T>
T Field(uintptr_t base, uint32_t offset)
{
    return *reinterpret_cast<const T*>(base + offset);
}
} // namespace

LoadingScreen CurrentLoadingScreen()
{
    __try
    {
        const uintptr_t logic = *reinterpret_cast<const uintptr_t*>(kGameLogic);
        const uintptr_t gmm = logic ? Field<uintptr_t>(logic, kModeManager) : 0;
        if (!gmm)
            return LoadingScreen::World;    // still starting up

        const bool worldLoaded = Field<uint32_t>(gmm, kCurrentEvent) != 0 &&
                                 Field<uint8_t>(gmm, kWorldReady) != 0;
        if (!worldLoaded)
            return LoadingScreen::World;
        if (Field<uint8_t>(gmm, kHasNextEvent) && !Field<uint8_t>(gmm, kIntroLoaded))
            return LoadingScreen::Event;
        return LoadingScreen::None;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return LoadingScreen::None;
    }
}

const char* LoadingScreenName(LoadingScreen screen)
{
    switch (screen)
    {
    case LoadingScreen::World: return "start/world";
    case LoadingScreen::Event: return "event";
    default:                   return "none";
    }
}
