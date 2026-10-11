# MW2012 FPS Unlock

An ASI plugin for **Need for Speed: Most Wanted (2012)** (PC, `NFS13.exe`
v1.5) that removes the 60 FPS cap, makes the game actually move at the higher
frame rate, and adds a **borderless window mode** so your monitor keeps its
full refresh rate.

The simulation keeps running at its fixed 60 Hz, so game speed, physics, AI
and timers are exactly as shipped. Only rendering is unlocked, and every
rendered frame shows the world blended to that moment. No game files are
modified.

## What it does

- **Unlocks the frame rate.** The game busy-waits for its 60 Hz simulation
  tick before every frame; the plugin replaces that wait with one that hands
  each frame only the ticks owed by real time.
- **Adds a borderless window mode.** In fullscreen the game always switches
  the monitor to 60 Hz, which alone caps what you can see at 60 frames per
  second. By default the plugin runs the game in a borderless window covering
  the screen instead: the monitor keeps the refresh rate Windows uses,
  Alt+Tab is instant, and the screen never goes black when switching. The
  game's own exclusive fullscreen is still available and then uses your
  desktop's refresh rate instead of 60 Hz.
- **Interpolates motion.** With the simulation at 60 Hz, extra frames would
  just redraw the same world. The plugin keeps the last two simulation states
  of everything the renderer reads per tick and draws the blend between them,
  restoring the true values before the simulation sees them. Covered:
  - all cars (player, traffic, police) and the camera / world view
  - the driver model
  - street lamps, small props, loose debris
  - pieces of broken props (lamp posts, signs, speed cameras)
  - wheels of traffic cars knocked into physics mode
  - car-attached effects (nitrous flame, rim sparks)
  - in-world readouts (speed camera and jump results, race position, car name)
- **Keeps the game stable uncapped.** At 60 fps the game's render worker
  threads always finished before the next frame reused their data; uncapped
  they do not. The plugin adds a worker barrier at those points, bounds checks
  on two render structures the game indexes without checks, a crash guard
  that turns the remaining torn reads into one-frame glitches, and limits the
  DXGI render-ahead queue to one frame.
- **Keeps world streaming in the game's original order.** The game frees the
  memory of unloaded map sections at the end of a frame and removes their
  objects in the next simulation tick. At 60 fps a tick always came first;
  uncapped, frames were drawn in between from freed memory (freezes and
  crashes in tunnels and other streaming-heavy places). Streaming now only
  steps when the next frame is certain to start with a tick.

## Requirements

- Need for Speed: Most Wanted (2012), `NFS13.exe` **v1.5** (13,684,056 bytes,
  file version 1.5.0.0; Steam and Origin/EA app). Other versions are
  rejected with a message box. The v1.5 disc exe may work too but is
  untested; if its code differs, the plugin shows a message box and patches
  nothing.
- An ASI loader: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  (v9.7.4 is included in the release download; v4.68, the version
  NFSMW2012HDEffects ships, works too), set to
  `DontLoadFromDllMain=0` in `scripts\global.ini` (also included).

## Installation

### Download (recommended)

**[Download the latest release](../../releases/latest)** - no building needed.

1. Open the `.zip` from the release page.
2. Drag its contents into your game folder (the one containing `NFS13.exe`)
   and allow Windows to merge the `scripts` folder.
3. Start the game normally.

The release archive already contains the ASI loader and its settings, so the
game folder ends up with:

```
<game folder>\dinput8.dll                     Ultimate ASI Loader v9.7.4
<game folder>\scripts\global.ini              loader settings
<game folder>\scripts\MW2012FpsUnlock.asi
<game folder>\scripts\MW2012FpsUnlock.ini
<game folder>\scripts\MW2012FpsUnlock-README.txt
<game folder>\scripts\MW2012FpsUnlock-LICENSE.txt
<game folder>\scripts\Ultimate-ASI-Loader-LICENSE.txt
```

To uninstall, delete those files again.

### Manual install (when building it yourself)

Copy `MW2012FpsUnlock.asi` (see *Building*) and `MW2012FpsUnlock.ini` into
`<game folder>\scripts\`, and install
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest)
(tested: v9.7.4 and v4.68) as `<game folder>\dinput8.dll` with this
`scripts\global.ini`:

```ini
[GlobalSets]
DontLoadFromDllMain=0
```

That setting is required for this game with any loader version: in its
default mode the loader crashes the game at startup, even with no plugin
installed (it hooks the same startup functions as the game's copy protection
and the EA app overlay).

If you already use [NFSMW2012HDEffects](https://github.com/AeroWidescreen/NFSMW2012HDEffects),
you already have a working loader (v4.68) and the same `global.ini`; letting
the archive replace them is fine, and both plugins work side by side with
either loader version. If another
mod installed a different `dinput8.dll`, you can keep it as long as
`scripts\global.ini` contains the line above.

## Configuration

`MW2012FpsUnlock.ini`:

| key | default | |
|---|---|---|
| `TargetFPS` | `0` | Frame cap, in any display mode. `0` = no cap, otherwise the maximum frames rendered per second (min. 30). |
| `LoadingScreenCap` | `1` | `1` = start and loading screens run at 60 fps instead of thousands, `0` = uncapped. |
| `Borderless` | `1` | `1` = borderless window covering the screen, `0` = the game's exclusive fullscreen. |
| `RefreshRate` | `0` | Monitor refresh rate in exclusive fullscreen. `0` = the same as your Windows desktop, otherwise the closest rate your monitor supports. In borderless mode the monitor runs at your Windows setting. |
| `VsyncInterval` | `-1` | Vsync. `-1` = the game's own setting, `0` off, `1` on. |
| `LogLevel` | `1` | `0` off, `1` normal, `2` verbose (adds a statistics line every minute). |

### Display mode

In the default borderless mode Windows composes the game like any other
window: you see one new frame per screen refresh (170 frames a second on a
170 Hz monitor) and never any tearing, while the game keeps rendering as fast
as it can. To change the screen's refresh rate, change it in Windows.

With `Borderless=0` the game uses its exclusive fullscreen (Alt+Enter). That
skips the Windows compositor - frames beyond the refresh rate show up as
tearing, and switching in and out of the game blanks the screen for a moment.

Leave the game's own vsync off. If you want a cap, set `TargetFPS` or use your
graphics driver's frame limiter.

## Reporting a problem

The plugin writes `MW2012FpsUnlock.log` next to the `.asi`, or to `%TEMP%`
when that folder is not writable (the case for a default install under
Program Files). The previous session's log is kept as
`MW2012FpsUnlock.log.old`. It records the version, your settings, which
patches were applied, and a report with registers and call stack if the game
crashes.

To report a problem, [open an issue](../../issues/new/choose) and attach that
log.

## Known limitations

- Very rarely the game can still crash. The remaining crash seen in testing
  happens inside the NVIDIA D3D11 driver on one of its own threads.
- In very long uncapped sessions (hours) memory use grows slowly inside the
  graphics driver. A `TargetFPS` below your uncapped frame rate keeps it flat.
- If the game closes right at startup after installing, check that
  `scripts\global.ini` exists and contains `DontLoadFromDllMain=0` (see
  *Manual install*).
- Not compatible with other frame limiters or frame rate unlockers for this
  game.
- Some antivirus programs flag ASI plugins and `dinput8.dll` loaders in
  general, because they load into the game and patch its code. The plugin is
  built from this repository by GitHub Actions; you can check the workflow
  run of each release or build it yourself.

## Building

Visual Studio 2022 or newer, Desktop development with C++ workload,
**Release | Win32**:

```
msbuild MW2012FpsUnlock.sln -p:Configuration=Release -p:Platform=Win32
```

Output: `build\Release\MW2012FpsUnlock.asi`. With a newer toolset only, add
e.g. `-p:PlatformToolset=v145`.

`tools\package.ps1` turns that build into the release archive: it downloads
the pinned Ultimate ASI Loader release (checked against its SHA-256) and
writes `build\dist\MW2012FpsUnlock-v<version>.zip`.

GitHub Actions (`.github/workflows/build.yml`) runs both on every push and
pull request and keeps the archive as a workflow artifact. Pushing a tag that
matches `src/version.h` (e.g. `v1.0.0`) publishes a release with the archive
and `MW2012FpsUnlock.pdb` attached.

## Source layout

| path | |
|---|---|
| `src/dllmain.cpp` | exe check, configuration, installs everything in order |
| `src/pacer.cpp` | the frame pacer replacement (the unlock itself), frame cap, vsync |
| `src/barrier.cpp` | render worker barrier |
| `src/guards.cpp` | bounds checks on the render descriptors and the command replay |
| `src/crashguard.cpp` | fault emulation for torn render-object reads, crash reports |
| `src/framelatency.cpp` | DXGI render-ahead limit |
| `src/streamsync.cpp` | world streaming in step with the simulation ticks |
| `src/display.cpp` | borderless window, exclusive fullscreen refresh rate |
| `src/gamestate.cpp` | reads the game's loading state (for `LoadingScreenCap`) |
| `src/config.cpp`, `log.cpp` | INI settings, session log |
| `src/blend/frame.cpp` | per-frame timing of every blend: apply, then restore |
| `src/blend/objects.cpp` | entity transforms (cars, props, lamps) |
| `src/blend/view.cpp` | camera / world view |
| `src/blend/character.cpp` | driver model |
| `src/blend/readouts.cpp`, `player.cpp` | in-world readouts, player car detection |
| `src/blend/effects.cpp`, `wheels.cpp`, `pieces.cpp` | effects, physics-mode wheels, broken prop pieces |
| `tools/package.ps1`, `dist/` | release archive: loader download, `global.ini`, readme, licenses |
| `.github/` | build and release workflow, issue forms |

All game addresses are for `NFS13.exe` v1.5 (image base `0x400000`, not
relocated). Every patch site is checked against the expected bytes before
anything is written.

## Credits

- **[NFSMW2012HDEffects](https://github.com/AeroWidescreen/NFSMW2012HDEffects)**
  (AeroWidescreen): reference mod for the same exe. The entry-point values
  that identify the v1.5 exe come from it (credited there to thelink2012 and
  MWisBest); no code is reused.
- **[NFS Rivals Framerate Unlocker](https://github.com/Brawltendo/NFS-Rivals-Framerate-Unlocker)**
  (Brawltendo): reference for how an NFS frame rate unlock can keep gameplay
  intact (no code reused).
- **[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)**
  (ThirteenAG).
- **EA / Criterion Games** for the game.

Reverse engineered and written with AI assistance: GLM 5.3 in Kilo Code at
first, later Claude Code. Tools: IDA Pro with ida-pro-mcp, REA and RenderDoc.

## License

[MIT](LICENSE)
