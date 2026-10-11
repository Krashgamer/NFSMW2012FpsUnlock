#pragma once

// Which loading screen, if any, the game is showing. Read from the game's
// GameModeManager, through the same fields its own UI properties report
// (WorldLoaded, HasNextEvent, HasIntroLoadCompleted).
enum class LoadingScreen
{
    None,
    World,   // start screen and loading the open world: WorldLoaded is false
    Event,   // loading a race or other event: a next event is queued and its
             // intro has not finished loading
};

// Game thread only (called once per frame by the pacer).
LoadingScreen CurrentLoadingScreen();

const char* LoadingScreenName(LoadingScreen screen);
