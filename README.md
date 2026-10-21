# TS3FpsUnlock

An ASI plugin for **Toy Story 3: Complete Edition** (PC, Steam) that raises the 60 FPS cap to
**120 FPS**, or to any value from 30 to 360. Physics, gameplay and timers keep working the same.

The game already measures how much time each frame takes and moves everything by that amount.
That covers movement, animation, cloth and physics, and physics also splits long frames into
smaller steps. The port just never let it run faster than 60. The plugin only changes how frames
are paced and leaves gameplay code alone.

## Features

- **Configurable frame cap.** The game's "Frame cap 60" option now means `TargetFPS` (120 by
  default). "Frame cap 30" still gives 30.
- **Lower CPU use.** The game's own limiter waits by spinning in a loop, which keeps one CPU core
  at 100%. The new limiter sleeps on a high-resolution timer and only spins for the last
  millisecond. It also paces frames to fixed deadlines, so frame times stay even.
- **Optional VSync off** (`VSync=0`). Use it for G-Sync/FreeSync monitors, or to go above 60 FPS on
  a 60 Hz screen.
- **Experimental: smoother frame times** (`PreciseDeltaTime=1`). The game counts frame time in whole
  milliseconds, so at 120 FPS frames alternate 8/8/9 ms. This option gives the game the exact
  frame time instead.

## Installation

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
   (x64) if you don't have an ASI loader yet. Put its `dinput8.dll` in the game folder, next to
   `Toy Story 3 Complete Edition.exe`. The loader is not included in `TS3FpsUnlock.zip`.
2. Download `TS3FpsUnlock.zip` from the [Releases](../../releases) page and extract it into the same folder:
   ```
   Toy Story 3 Complete Edition\
   ├── Toy Story 3 Complete Edition.exe
   ├── dinput8.dll        ← Ultimate ASI Loader (step 1)
   ├── TS3FpsUnlock.asi
   └── TS3FpsUnlock.ini
   ```
3. Start the game. In **Options**, set **Frame cap** to **60**. A new profile starts at 30.
   The menu still says "60", but the plugin raises it to `TargetFPS`.

On startup the plugin creates `TS3FpsUnlock.log` next to the exe.

To uninstall, delete `TS3FpsUnlock.asi`, `TS3FpsUnlock.ini` and `TS3FpsUnlock.log`. Also delete
`dinput8.dll` if no other ASI plugin uses it. The plugin works alongside
[TS3Debug](https://github.com/GregoriousX/TS3Debug).

## Configuration

Edit `TS3FpsUnlock.ini` (section `[FPS]`) and restart the game.

| Option | Default | Description |
|--------|---------|-------------|
| `Enabled` | `1` | `0` turns the plugin off |
| `TargetFPS` | `120` | Frame cap used when the game's option is "Frame cap 60". Range 30–360 |
| `VSync` | `1` | `0` turns VSync off |
| `PreciseDeltaTime` | `0` | Experimental: exact frame time instead of whole milliseconds |
| `LogStats` | `0` | Write the average FPS to the log every 10 seconds |

### Picking `TargetFPS` and `VSync`

With VSync on, the frame rate also can't go above your monitor's refresh rate.

| Monitor | Recommended |
|---------|-------------|
| 60 Hz | `VSync=0` if you want more than 60 FPS |
| 120 / 144 / 165 Hz | `TargetFPS` = refresh rate, `VSync=1`. 120 FPS on a 144 Hz screen stutters |
| G-Sync / FreeSync | `VSync=0`, `TargetFPS` a few frames below the maximum refresh rate |

## Troubleshooting

- **No `TS3FpsUnlock.log` appears.** The ASI loader isn't running. Check that `dinput8.dll` is in
  the same folder as the exe.
- **The log says `unsupported game version`.** A game update changed the code. The plugin
  disables itself instead of patching the wrong place. Please open an issue and attach the log.
- **Still 30 FPS.** Set **Frame cap** to **60** in the game options.
- **Still 60 FPS.** Your monitor runs at 60 Hz and VSync is on. Set `VSync=0`, or check that
  Windows runs the monitor at its full refresh rate.
- **Is the frame rate what I set?** Set `LogStats=1`. The log then shows the measured average FPS.

## How it works

- All game addresses are found at startup by signature scan. Nothing is hardcoded. Each signature
  must match exactly one place. Otherwise that part of the plugin stays off and the log says why.
- The game's frame limiter function is replaced. The plugin still reads the game's own
  "Frame cap 30/60" setting, so switching it in the menu works at once.
- The cap is clamped to 360 FPS. The game's timer can't handle more than ~1000 frames per second,
  so an uncapped mode isn't offered.
- `VSync=0` changes one instruction: the sync interval the game passes to `Present`.
- `PreciseDeltaTime` lets the game's timer run as usual, then replaces its per-frame time step with
  one measured by a high-resolution timer. It does this only when that matches the game's own
  measurement. Pause, slow motion and other speed modes keep working.

## Building

You need Visual Studio 2022 Build Tools (or newer) with the C++ x64 toolset.

```bat
build.bat
```

The output goes to `bin\TS3FpsUnlock.asi`.

## Credits

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG
- Toy Story 3 © Disney/Pixar. This is an unofficial fan project and is not affiliated with Disney, Pixar or Avalanche Software.
