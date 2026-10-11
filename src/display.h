#pragma once

struct IDXGISwapChain;

// Display mode.
//
// The game builds its device and swap chain from one settings block (width
// +0x0C, height +0x10, format +0x14, refresh rate +0x40 / +0x44 as a rational,
// output window +0x54, fullscreen flag byte +0x61), handed to sub_D9BB20 at
// startup and to sub_D97D80 when the video settings change. Both are
// intercepted.
//
// Borderless (default): the swap chain is always created windowed and the
// game window becomes a borderless window covering its monitor. The display
// mode is never touched, so switching in and out of the game is instant, and
// the screen runs at whatever refresh rate Windows uses.
//
// Exclusive fullscreen (Borderless=0): the game asks for ~60 Hz whatever the
// monitor supports, which alone caps what is visible at 60 fps. The refresh
// rate is replaced by the monitor mode closest to the Windows desktop's (or
// the INI's RefreshRate).
bool DisplaySitesOk();
void InstallDisplay();

// Once the game's swap chain exists (framelatency.cpp).
void OnSwapChain(IDXGISwapChain* swapChain);
