#pragma once

// Settings from MW2012FpsUnlock.ini, next to the .asi. Missing keys keep the
// defaults below.
struct Config
{
    int  targetFps     = 0;     // 0 = uncapped, otherwise a frame cap (>= 30)
    bool capLoading    = true;  // 60 fps on start and loading screens
    int  vsyncInterval = -1;    // -1 = leave the game's own setting alone
    bool borderless    = true;  // borderless window instead of exclusive fullscreen
    int  refreshRate   = 0;     // exclusive fullscreen Hz; 0 = the Windows desktop's
    int  logLevel      = 1;     // 0 off, 1 normal, 2 verbose
};

extern Config g_config;

void LoadConfig();
