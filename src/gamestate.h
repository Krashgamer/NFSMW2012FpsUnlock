#pragma once

// Which screen without gameplay, if any, the game is showing. World and Event
// are read from the game's GameModeManager, through the same fields its own
// UI properties report (WorldLoaded, HasNextEvent, HasIntroLoadCompleted);
// NoWorld from the game object's world fade.
enum class LoadingScreen
{
    None,
    World,    // start screen and loading the open world: WorldLoaded is false
    Event,    // loading a race or other event: a next event is queued and its
              // intro has not finished loading
    NoWorld,  // the world is faded out behind a full-screen screen: the
              // loading screen when switching cars, the map
};

// Game thread only (the pacer and the streaming step, once per frame each).
LoadingScreen CurrentLoadingScreen();

const char* LoadingScreenName(LoadingScreen screen);
