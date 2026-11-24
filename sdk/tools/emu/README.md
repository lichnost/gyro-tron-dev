# The emulator

**Status:** reference - as-built for this repo; `emu.py` itself and the emulator
chapter of the root README are the authority for the flags.

Run a Gyro-Tron game on a PC: in a window, to play it, or from a script, to test it.

```bash
python tools/emu/emu.py play skatergirlz
python tools/emu/emu.py shot echo "wait 0.5; tap 233 233; wait 2; shot turn.png"
python tools/emu/emu.py load gbboy
```

The third one is not a game running - it is the watch's own package loader checking a `.tat`
on the PC (see "What it is not", below).

The console's own screens run here too, though they are not games: `@boot` is the boot
animation, `@power` the power menu, `@hold` the power menu as the hold brings it up
(`emu.py shot @hold "hold b; wait 2; shot ring.png; wait 1.5; shot menu.png"`), and `@home`
the launcher. The launcher shows the battery the power chip reports; on a PC that is the
environment, read once at start: `EMU_BATTERY` percent (default 62), `EMU_CHARGING=1`,
`EMU_USB=1`. So:

```bash
EMU_BATTERY=55 EMU_CHARGING=1 python tools/emu/emu.py shot @home "wait 1; shot chg.png"
```

`@update` is the firmware-update screen with its pull-OTA client faked: `EMU_OTA` says
what the site has (`available` - an update to download and install; `ready` - one already
downloaded; `uptodate` or unset - nothing new; `failed` - the check fails), `EMU_USB=1`
plugs the cable in, which is what the install button asks for. The watch's real network
stack does not exist here; what is under test is the screen's states and their rules:

```bash
EMU_OTA=available EMU_USB=1 python tools/emu/emu.py shot @update \
  "wait 1; tap 233 133; wait 1.5; tap 233 133; wait 0.3; tap 161 391; wait 4.6; shot ready.png"
```

`@store` is the game store with its catalog fetcher faked: `EMU_STORE` names a local
folder holding `index-watch.json` (the compact catalog, docs/reference/CATALOG.md) and
the packages, named by their catalog-relative paths (`games/official/echo.tat`). An
index fetch reads the file after a short delay; a package "download" copies it into
`Games/` over about a second, progress on the ring. Installs are remembered for the
session, so a script can walk the whole flow - list, detail, install, back to the list
with the game marked installed:

```bash
EMU_STORE=build/emu/store-mock python tools/emu/emu.py shot @store \
  "wait 1.5; tap 233 125; wait 0.5; shot detail.png; tap 153 390; wait 2.5; shot installed.png"
```

`@cal` is Settings > CALIBRATE straight up, for scripted runs of the calibration flow
(docs/reference/CALIBRATION.md). The synthetic sensor is perfect by default, and a perfect sensor
cannot be calibrated - every pose reads the same gravity - so the flow's fit correctly
reports degenerate coverage and asks for more. Fault injection gives it something to
find, in the frame the firmware receives: `EMU_ACC_OFFSET=x,y,z` (g), `EMU_ACC_GAIN=x,y,z`
(what the part reads for 1 g), `EMU_GYRO_BIAS=x,y,z` (deg/s). A full pass, then the check
that the saved correction lands - the same raw sample straightening out to true gravity:

```bash
EMU_ACC_OFFSET=0.03,-0.02,0.05 EMU_ACC_GAIN=1.02,0.97,1.05 EMU_GYRO_BIAS=1.5,-2.5,0.5 python tools/emu/emu.py shot @cal "wait 0.5; tap 305 391; pitch -10; wait 2; pitch -90; wait 2.5; yaw 45; wait 0.05; yaw 90; wait 0.05; yaw 135; wait 0.05; yaw 180; wait 2.5; pitch -45; wait 1.5; turn 90; wait 2.5; turn 180; wait 2.5; turn 270; wait 2.5; turn 360; wait 2.5; wait 1; tap 305 391; wait 2; shot result.png" --define CAL_BENCH --fresh
```

The `CAL_BENCH` define adds a once-a-second log line - `bench raw=(..) g=(..) view=(..)` -
raw sensor against what a game would see, which is the assertable end-to-end signal, plus
per-reading misfits on the result screen. Run the same command without `--fresh` and the
bench shows the saved correction straightening the sensor from the first frame. The
background refresher (engine/autocal.h) is wired to the same screen: park the watch on
several poses for ~9 s each after a calibration, wait out the five-minute refit throttle,
and `wait_log "refit ok"` catches the quiet re-fit.

Two script details the flow needs: a jump of exactly 180 degrees is invisible to the
gyro - the rotation axis cannot be recovered from one frame - so half turns go as
90-degree steps with a frame between. And a flat watch spun about the vertical sees no
gravity change at all, because gravity rides the spin axis: the flow's spin ring is held
TILTED, and `turn` is what sweeps it, not `yaw`.

The theme behind the launcher is the built-in one here - theme files are read from the
watch's drive, which this does not have.

You need Python 3 with Pillow, and a C++ compiler: gcc or clang, or on Windows the MSVC build
tools, which it finds by itself. The first run of a game builds it, in about three seconds;
after that it rebuilds only when a source or one of the game's own headers has changed.

Every link also writes a linker map next to the library (`build/emu/<game>/<game>.map`),
and `tools/emu/sym.py` turns a crash address into the function it landed in:

    python tools/emu/sym.py <game> 0x180012A40        # game id, absolute address
    python tools/emu/sym.py <game> 12A40 --rva        # the RVA a fault handler prints
    python tools/emu/sym.py <game> 0x7FF9... --base 0x7FF9...   # a moved DLL's load base

## What it is

Not a lookalike. A game is plain C that reaches the console through one table of functions
(`tat_api.h`), and the console's side of that table is ordinary C++ with very little hardware
in it. So the emulator compiles **the firmware's own sources, unchanged** - the pixel canvas,
the font, banners, the pause menu, the save store, the whole game API
(`components/tat_api/tat_host.cpp`) - together with the game, into one library, and replaces
only what is underneath:

| On the watch | Here |
| --- | --- |
| the presenter streams bands of pixels to the panel | it collects them into a picture |
| an input task reads touch, buttons and the motion sensor | the front end says what they read |
| the clock is the chip's | the clock advances by exactly the step it is given |
| sound goes to the codec | each tone is a line in the log; a game's PCM stream piles up and lands in `build/emu/<game>/audio.wav` at exit - played live too, when `sounddevice` is installed |
| saves go to NVS | saves go to `build/emu/<game>/save.txt` |

What a game draws here is what it draws on a watch, to the pixel, and a game that misbehaves
here misbehaves there. `emu_core.cpp` is the whole of the replacement; `stubs/` is the handful
of ESP-IDF headers the firmware's sources include.

## What it is not

- **It cannot tell you how a game feels in the hand.** Tilt from arrow keys is not tilt from a
  wrist. It will show you that the controls work, not that they are good.
- **It says nothing about speed.** A PC is hundreds of times faster than the watch.
- **Play and shot do not exercise the loader.** A game is linked in here, not loaded from a
  `.tat`. The `load` verb does exactly that - the real `components/loader/loader.c` against a
  package, reporting what a watch would log and show as THIS GAME WOULD NOT LOAD (needs gcc
  or clang; no xtensa). Make it part of finishing a game: `python tools/emu/emu.py load
  <game-or-file.tat>`.
- The gyro is derived from how the pose moved since the last frame, not measured. It is
  exact for scripted and keyboard turns and silent when nothing moves - good enough to
  develop an orientation filter against, not a model of a real IMU's noise. The frame
  rotation a filter should turn by (`tilt.rx/ry/rz`) is the same derivation read as an
  integral, so it is exact where a real part's `gx*dt` is not.

## Playing

`play` opens a round window the size of the watch's screen.

| | |
| --- | --- |
| mouse | your finger |
| left / right | turn the watch like a wheel (shift: finer) |
| up / down | tip the top toward you (glass to the ground) / back upright |
| A / D | look around the vertical axis (yaw), the way a scope game aims |
| space | level |
| F | lay the watch flat, or hold it upright again (it starts upright, which is how it is held) |
| B | the PWR button |
| P | save a picture to `build/emu/<game>/` |
| Esc, H | BOOT: home, which here is quit |

`play <game> --record clip.mp4` films what is played (it needs ffmpeg): every frame, pixel for
pixel at 2x on a black 1080 square, at exactly sixty frames a second of the game's own time, so
the film is smooth even where the window was not.

## Testing

`shot` has no window. It runs a script at exactly sixty frames a second of its own time, with
its own dice, so **the same script gives the same pictures every time** - which is the point.
Steps are separated by `;` or newlines, and the script can be a file.

```
wait <seconds>                       tap <x> <y>
touch <x> <y>  /  release            swipe <x0> <y0> <x1> <y1> [seconds]
turn <degrees>    upright, turned like a wheel, right positive
pitch <degrees>   and tipped, top toward you positive - the glass tips to face
                  the ground (aiming down), so a game's sight-line pitch reads negative
yaw <degrees>     and turned about the vertical (looking around); gravity does not move,
                  the gyro does - a jump is one frame of fast turning
flat <1|0>        laid on the table (1) or held (0); pitch stacks on top of it
tilt <ax> <ay> <az>                  gravity as the game sees it, in g
button b [seconds]                   PWR
hold b  /  letgo                     PWR down and left down
shot <file.png>                      sheet <file.png>   every shot so far, tiled
wait_pixel <x> <y> <#hex> [tol] [s]  run frames until the pixel is within tol of the
                                     color (default tol 8, timeout 10 s) - scripts keyed
                                     to what is on screen instead of one seed's timings
wait_log "<regex>" [seconds]         run frames until the game has logged a matching
                                     line (default timeout 30 s; works with --quiet)
```

A `wait_pixel`/`wait_log` timeout exits nonzero with the reason, so a scripted run fails
loudly instead of capturing the wrong moment. The game's log reaches the script even under
`--quiet`: the library piles lines up (like its PCM stream) and `Console.pump_log()` drains
them into `log_lines`.

`build <game> [game ...]` compiles without opening a window - the fast "does it
still build" check (what the dev plugin's `compile_game` calls). `load <file.tat>`
(or a game id with a `build/<id>.tat`) builds nothing: it hands the package to the
firmware's real loader and prints what the loader decides - no execution. `play
<game>` also takes `--zoom N` for a bigger window; `shot` takes `--asset NAME=PATH`
(repeatable) to stand a local file in for one of the game's packaged assets.

From Python, `Console(game, defines=("SK_AUTOPILOT",))` builds the game with a test switch
of its own turned on, as a separate library. Both `play` and `shot` take the same switches
on the command line: `--define NAME` (repeatable).

`--seed N` changes the dice, `--fresh` forgets the saved scores first, `--quiet` drops the log
from the console (the game's log still piles up for `wait_log` and `pump_log()`).
The log is the same one the watch serves at `/log`: whatever the game says with `T->log()`.

It can also be driven from Python, which is how a test that needs to *react* to a game is
written - `Console` in `emu.py` has `step()`, `run(seconds)`, `pose()`, `touch`, `held` and
`image()`.

## Memory pressure

Both are off by default, which keeps goldens byte-exact.

- `EMU_HEAP_MB=<n>`: a game's own heap shim replaces libc's malloc, with an arena
  of `n` MB taken through `api->alloc`. Exhaustion logs `<game>: heap exhausted at N bytes`
  and exits 3. The engine needs about 32 MB here, as 64-bit pointers inflate it.
- `EMU_PSRAM_MB=<n>`: a plain byte budget under `heap_caps_*`, with no fragmentation model.
  Refused asks reach the eviction hook and step-down ladders. Prints
  `psram: budget B used U peak P refused R` at exit. With `EMU_HEAP_MB` above it, the arena
  is assembled from segments as on the watch.
