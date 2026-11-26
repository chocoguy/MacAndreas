# MacAndreas

## A native static recompilation port of GTA SA for macOS

![Logo](https://mangogravy.com/MacAndreasIMPROVED-small.jpg)

MacAndreas converts your own copy of GTA: San Andreas (PC, `gta_sa.exe` 1.0 US) into a native
Apple Silicon program. SilentPatch, the Widescreen Fix and Project2DFX are built in. This
repository contains no game code or assets; see [ROADMAP.md](ROADMAP.md) for the plan.

Status: **M6 (playable vanilla and the launcher) in progress.** The game boots through the intro
movies to the main menu, is playable in Los Santos with keyboard and mouse, and has sound. It runs
windowed, borderless or in native full screen on any display, captures the mouse for mouse look,
and saves and loads. A SwiftUI launcher verifies the game, converts it and starts it. Not done
yet: mission playthroughs, a long soak test, Windows save round trips and a clean-Mac run.
Controller support exists but is untested with real controllers; it moved to M11.

### Requirements

- Apple Silicon Mac, macOS 26 or later
- Xcode command line tools, CMake 3.28+, Ninja
- Ghidra 12.2 (developers regenerating `analysis/` only)

### Build

```bash
git config core.hooksPath .githooks
```

```bash
cmake --preset dev
```

```bash
cmake --build --preset dev
```

```bash
ctest --preset dev
```

The recompiler tests also need Remill built against Homebrew LLVM 22
(`brew install llvm@22`). This clones Remill at a pinned commit into the git-ignored `.cache/`:

```bash
tools/recomp/setup-remill.sh
```

Tests labelled `game` need your own executable. By default they look in
`gamefiles/Grand Theft Auto San Andreas/`, which is git-ignored. Set `MACANDREAS_GAME_DIR` at
configure time to point elsewhere. Without the game they are skipped.

### Launcher

The build also produces `build/dev/launcher/MacAndreas.app`. It bundles `sa-host`, `sa-verify`,
`sa-recomp` with its own copy of LLVM, the manifest, the guest runtime and, if you have exported
it, `analysis/sa-1.0us`. It does the following:
- remembers the game folder and checks `gta_sa.exe` with `sa-verify`;
- converts the game into `~/Library/Application Support/MacAndreas/build` with a progress bar and
  writes `stamp.json` (hashes of the executable, analysis data, manifest, guest runtime and
  `sa-recomp`), converting again only when one of them changes;
- offers the display, window mode, aspect ratio, resolution and intro movies;
- launches `sa-host`, logging to `~/Library/Logs/MacAndreas/`.

```bash
open build/dev/launcher/MacAndreas.app
```

It also runs without a window, which is how scripts and tests drive it:

```bash
build/dev/launcher/MacAndreas.app/Contents/MacOS/MacAndreas --headless --game "gamefiles/Grand Theft Auto San Andreas" status
```

`convert` runs the conversion and `snapshot FILE.png` renders the launcher window to an image.

### Recompile and boot

`analysis/` is local-only, so export it first with `tools/recomp/ghidra/export.sh`. Then lift,
compile and link the executable into `build/dev/native/libgta_sa_native.dylib`. This takes
about 11 minutes:

```bash
build/dev/tools/recomp/sa-recomp/sa-recomp build --exe "gamefiles/Grand Theft Auto San Andreas/gta_sa.exe" --analysis analysis/sa-1.0us --manifest manifest --runtime build/dev/runtime/recomp/guest_intrinsics.bc --out build/dev/native
```

After a compiler-only change, rebuild the objects from the existing shards without lifting
again:

```bash
build/dev/tools/recomp/sa-recomp/sa-recomp recompile --runtime build/dev/runtime/recomp/guest_intrinsics.bc --out build/dev/native
```

Boot it. `--trace-imports` logs every API call:

```bash
build/dev/host/sa-host --game "gamefiles/Grand Theft Auto San Andreas" --native build/dev/native/libgta_sa_native.dylib --manifest manifest
```

Display and debugging options:
- `--resolution WxH` sets the starting display mode (default 1280x720, in pixels). The game's
  Resolution option offers it and standard modes that fit the chosen display; `--modes WxH,…`
  replaces those. A saved `gta_sa.set` takes precedence over `--resolution`.
- `--window-mode windowed|borderless|fullscreen` picks a window (default), a borderless window
  covering the display (the menu bar and Dock hide, and the notch area stays black), or native
  macOS full screen in its own Space. Control-Command-F toggles native full screen from windowed
  mode. The picture keeps its aspect ratio and is letterboxed or pillarboxed to fit.
- `--display N` puts the window on display N, counted in `NSScreen` order with 0 being the display
  that holds the menu bar.
- The mouse is captured while the game hides the pointer and its window is focused: the pointer is
  frozen and hidden, mouse look gets raw motion, and the menu pointer is confined to the picture.
  Press Command (or Command-Tab away) to release it; click the game to capture it again.
- Cmd-Q asks the game to close, as its close button does; a second Cmd-Q quits at once.
  `--no-mouse-capture` turns this off. Scripted runs with `--ignore-user-input` never capture.
- `--frame-times FILE.csv` records the time between presented frames and prints a summary
  (`[PERF]`) on exit. `tools/perf/frame_report.py FILE.csv --skip N` prints percentiles and a
  histogram, ignoring the first N frames.
- `--skip-intro` skips the intro movies.
- `--capture-dir DIR --capture-frames N,M,…` (or `all`) writes presented frames as PNG. Movie
  frames are numbered separately and saved as `movie_NNNNN.png`.
- `--exit-after-frames N` quits after N presented frames.
- `--keys FRAME:ACTION,…` scripts input for unattended runs. `FRAME` is a presented game frame;
  `FRAMEm` counts movie frames and `Ns` counts seconds since start. Actions are key names in lower
  case (`return`, `escape`, `space`, `up`, `a`, `f1`, `lshift`, `num5` …), `lbutton`, `rbutton`,
  `mbutton`, `wheelup`, `wheeldown`, `move` and `focus`/`blur`. Keys and buttons tap by default;
  `+`/`-` hold and release. `@X:Y` first moves the pointer to client pixel X,Y. `look@DX:DY`
  sends a relative mouse motion, which turns the in-game camera. A tapped key stays down until the
  game has presented a frame, so typed cheats register. A virtual controller takes `pad.a`, `pad.b`,
  `pad.x`, `pad.y`, `pad.lb`, `pad.rb`, `pad.lt`, `pad.rt`, `pad.back`, `pad.start`, `pad.ls`,
  `pad.rs` and `pad.up`/`down`/`left`/`right` (tap, `+` or `-`), and `lstick@X:Y`/`rstick@X:Y` with
  X and Y in percent, up positive. `save1` … `save8` save to that slot through the same routine as
  the in-game save menu; the save is refused while a mission is running, because a mission's
  script state cannot be restored on load.
- `--ignore-user-input` ignores the real keyboard and mouse (Cmd-Q still quits), so a scripted
  run cannot be disturbed.
- `--ignore-focus-loss` keeps the game running when the window loses focus. By default it pauses
  as the original does, which stalls unattended runs.
- `--no-audio` mutes the output. The mixer still runs against the system clock, so streaming and
  cutscene timing behave as with a device.
- `--audio-capture FILE.wav` records the final mix (48 kHz, 16-bit stereo).
- `--dump-shaders DIR` writes the disassembly of every vertex and pixel shader the game creates.
- `--interpret ADDR,…` runs those traces in the interpreter instead of the lifted code, and
  `--trace-pc ADDR,…` logs registers and the x87 stack whenever the interpreter reaches them.
  `--interpret-range LO-HI,…` does the same for every trace in an address range, to bisect a
  suspected lifting bug. Direct calls between lifted functions bypass it.
- `--trap-nan` logs the first time each interpreted instruction puts a NaN on the x87 stack.
- `--watch ADDR,…` logs a guest 32-bit value whenever it changes, once per presented frame.
- `--shadow LO-HI,…` compares lifted code with the interpreter on real game state. On a call to a
  lifted function in the range, two forked children run it, one lifted and one interpreted, with
  guest memory write-protected so that every page written is recorded. Registers, the x87 stack
  and the bytes written are compared, and a `[SHADOW]` line reports any difference.
  `--shadow-from FRAME`, `--shadow-samples N` (per function, default 8), `--shadow-every N` and
  `--shadow-deep` (interpret the callees too) tune it. A child that calls an import with side
  effects (files, threads, waits, D3D, DirectSound, DirectInput) stops and the sample counts as
  inconclusive, so the game's own state is never touched.
- `--probe ADDR,…` logs the first calls to those functions with their arguments and return value,
  and counts calls every 300 frames. `--touch VALUE LO-HI,…` logs calls in the ranges whose
  registers or first six stack arguments hold VALUE. A VALUE below `0x10000` is a model index
  and matches any `CObject` of that model. Both honour `--shadow-from`.
- `--dump-memory FRAME:ADDR:SIZE:FILE` writes guest memory to a file at a presented frame (hex
  address and size).
- `--pool-stats` logs the ped, object, dummy and building pools every 300 frames, with how many
  entities are within 30 m of the player and how many of those have no collision data.
  `--pool-detail FRAME,…` lists each of those entities at the given frames.
- Set `MTL_DEBUG_LAYER=1` to run with Metal API validation.

For example, to capture the title screen and the main menu:

```bash
build/dev/host/sa-host --game "gamefiles/Grand Theft Auto San Andreas" --native build/dev/native/libgta_sa_native.dylib --manifest manifest --skip-intro --capture-dir out/frames --capture-frames 700,1000 --exit-after-frames 1000
```

Settings and saves live in `~/Library/Application Support/MacAndreas/Documents/GTA San Andreas
User Files`. A `gta_sa.set` written by an M2 build may hold the windowed desktop mode; delete it
once if the game switches to windowed mode after the splash screens. The game always sees one
controller, but a `gta_sa.set` saved before M5 has an empty joypad table that overrides the
default buttons; use Controller Setup → Restore Defaults or delete the file once.

To choose Start Game → New Game and capture the loading screen:

```bash
build/dev/host/sa-host --game "gamefiles/Grand Theft Auto San Andreas" --native build/dev/native/libgta_sa_native.dylib --manifest manifest --skip-intro --keys 1000:return,1100:return --capture-dir out/frames --capture-frames 1050,1350 --exit-after-frames 1350
```

To open Options with the mouse and capture the page:

```bash
build/dev/host/sa-host --game "gamefiles/Grand Theft Auto San Andreas" --native build/dev/native/libgta_sa_native.dylib --manifest manifest --skip-intro --keys 1000:lbutton@640:338 --capture-dir out/frames --capture-frames 1060 --exit-after-frames 1060
```

Code the recompiler missed runs in the interpreter and is logged as `[INTERP] 0xADDR`. To turn
those addresses into lift roots for the next build:

```bash
tools/recomp/interp_triage.py --apply boot.log
```

### In-world route

`tools/route/m4_route.py` starts a new game and plays a scripted 10-minute route. It rides the
opening mission's BMX, drives the Rhino, walks, sprints, flies the jetpack and runs the clock
through day and night. It captures every 300 frames into `out/route` and exits non-zero on a crash
or any `[INTERP]` hit. SA steps its physics by measured time, so run it on an idle machine:

```bash
tools/route/m4_route.py --minutes 10 --capture-dir out/route --log out/route.log
```

### Audio route

`tools/route/m5_route.py` cycles every radio station in Options → Audio Setup, then starts a new
game, plays the intro cutscene and rides the opening mission's BMX. It records the mix to
`out/route5.wav` and exits non-zero if a station or the cutscene stream never opens, the
recording goes silent, the game crashes or anything is interpreted. It is muted unless you pass
`--speakers`:

```bash
tools/route/m5_route.py --capture-dir out/route5 --log out/route5.log --audio out/route5.wav
```

### Save and load route

`tools/route/m6_save.py` starts a new game, checks that a save during the opening mission is
refused, fails the mission with the suicide cheat, saves to slot 8 after the hospital respawn and
checks the file with `tools/save/sa_save.py`. A second run loads the slot from Start Game → Load
Game and checks that the player's money is restored. An existing save in that slot is put back
afterwards:

```bash
tools/route/m6_save.py --capture-dir out/route6
```

`tools/save/sa_save.py GTASAsfN.b …` checks the size, block tag and checksum of any PC save and
prints its title and camera position.

### Differential x87 tests

`--diff-test LIST` runs each function in LIST (one hex address per line) both as lifted code and
in the interpreter, from the same randomized registers, stack arguments and buffers. It then
compares the registers, the x87 stack and every byte written. Each run happens in a forked child,
so faults and endless loops are contained. The x87 control word cycles through double, single and
extended precision. `--diff-iterations N` (default 12), `--diff-seed N` and `--diff-verbose`
adjust it. `--diff-x87-args N` pushes N random values (with ±1, 0 and ±0.5 mixed in) on the x87
stack for routines that take their argument there, such as the CRT's `_CI*` helpers.
`--diff-deep` also interprets the callees instead of handing them to lifted code. `tools/recomp/x87_candidates.py` lists the leaf functions that use the x87 unit:

```bash
tools/recomp/x87_candidates.py --exe "gamefiles/Grand Theft Auto San Andreas/gta_sa.exe" --functions analysis/sa-1.0us/functions.json --out analysis/sa-1.0us/x87_leaf.txt
```

```bash
build/dev/host/sa-host --game "gamefiles/Grand Theft Auto San Andreas" --native build/dev/native/libgta_sa_native.dylib --diff-test analysis/sa-1.0us/x87_leaf.txt
```

### Tools

- `sa-verify <gta_sa.exe | game dir>` checks that the executable is the supported 1.0 US build.
- `pe-inspect [--json] <file.exe>` prints headers, sections and imports.
- `tools/recomp/ghidra/export.sh <gta_sa.exe>` regenerates `analysis/sa-1.0us/`.
- `tools/repo-guard/repo_guard.py` blocks game files, lifted code and binaries from commits.

### License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
