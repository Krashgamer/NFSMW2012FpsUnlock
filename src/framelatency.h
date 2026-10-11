#pragma once

// Limits the DXGI render-ahead queue to one frame once the game's device
// exists. The default of three frames queue-ahead never happened under the
// 60 fps cap; uncapped it does, and with it came a crash inside the NVIDIA
// D3D11 driver (nvwgf2um.dll, on a driver thread). Runs on its own thread.
void StartFrameLatencyLimit();
