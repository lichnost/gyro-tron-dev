---
name: gyrotron-build-test
description: |
  Use when building, testing, debugging or visually checking a Gyro-Tron game - any
  "compile it", "run it in the emulator", "take a screenshot", "why doesn't it work",
  or a code-fix-retry loop on a game in games/<id>.
---

# The build-test loop

You own this loop. Run it as many times as needed without asking; show the user the
result, not the intermediate failures (unless you are stuck).

## The loop

1. `compile_game(game_id)` - the fastest honest build check (the emulator compiles the
   game's real C against the firmware's own canvas, font and API host; no xtensa needed).
2. `screenshot(game_id, script)` - runs the game headless, deterministically, and writes
   PNGs; **Read the PNG paths it returns and actually look at them.** You have vision;
   judging "does this look right" is your job, the tool only captures.
3. Fix, repeat. Three failed fixes on the same symptom: reread the API in
   `tat://api/header` and an example in `tat://examples/<id>` before trying again.

## Screenshot scripts

Screen is 466x466, origin top-left, centre (233, 233). Steps, `;`-separated:

```
wait <s>                tap <x> <y>           touch <x> <y>  /  release
swipe <x0> <y0> <x1> <y1> [s]                 button b [s]          hold b / letgo
turn <deg>              pitch <deg> (+ looks down)                  yaw <deg> (look around)
tilt <ax> <ay> <az>     (gravity in g)
shot <file.png>         sheet <file.png>      (every shot so far, tiled)
wait_pixel <x> <y> <#hex> [tol] [timeout_s]   run frames until the pixel matches
wait_log "<regex>" [timeout_s]                run frames until the game logs a match
```

Same script + same `seed` = same pixels every time - that is what makes a shot a
regression test. Saves persist between runs in `build/emu/<game>/save.txt`; pass
`fresh: true` to start clean. `env: {EMU_BATTERY: "55", EMU_CHARGING: "1"}` sets the
battery the console screens show.

Prefer `wait_pixel`/`wait_log` over hardcoded `wait`s: scripts keyed to what is on
screen (or to the game's own log lines) survive retiming and reroll tweaks; fixed
waits break whenever the game's RNG consumption or state timing moves (the duckhunt
suite's fixed waits produced two false failures and needed retuning after every
tweak). A `wait_*` timeout fails the run loudly instead of capturing the wrong
moment - that is the point.

## Golden regression

Once a shot looks right, make it an assertion: `screenshot(..., golden: true)` compares
every shot against `games/<id>/tests/golden/<name>.png` (mismatch count, first
mismatching pixel, and a diff mask PNG showing what moved). Re-baseline after an
intended change with `update_golden: true`. Always pin `seed` in golden runs; the
default tolerance is 0 because the emulator is deterministic. A suite of golden shots
is the difference between "checked by eyeballing" and "checked".

## Telemetry and autopilot before tuning

For a game where aiming, tilt or probabilities matter, build the instrumentation
FIRST, then tune gameplay numbers by reading it - blind angle-tuning through
screenshots wastes hours (the duckhunt lesson). The pattern: game-owned preprocessor
switches built through the tool's `defines` argument (a separately named library, so
a test build is never mistaken for the game):

- a telemetry define (`DH_DEBUG`-style) that logs positions/aim/state a few times a
  second through `T->log()` - `wait_log` then asserts on it, and the returned log
  carries the trace;
- an autopilot define (`DH_AUTOPLAY`-style) that plays end-to-end rounds and prints
  hit stats - for soak runs (`autoplay for N minutes, tally outcomes`) that would
  otherwise need a human.

Neither define ships: the packaged build never gets `-D` flags.

Console screens (not games) work too: game_id `@boot`, `@power`, `@hold`, `@home`,
`@update` - useful when your game touches the launcher's world (icons, saves, sleep).

## What the emulator can and cannot tell you

It is pixel-exact for drawing, honest about API misuse (the firmware's own code runs), and
its log echoes every `T->log()` the game makes - read it for state traces. It cannot tell
you how tilt *feels* in a hand, how fast the watch runs it, or whether the loader accepts
the `.tat` - for the first two, say so in your report; for the last, use
`build_package` + `validate_package` (the **gyrotron-ship** skill).

## Memory pressure

Allocation bugs can be invisible on the PC and crash the watch. Two environment knobs,
both off by default (goldens stay byte-exact):

- `env: {EMU_HEAP_MB: "32"}` (C-Dogs) runs the game's real heap shim instead of libc,
  with an arena of that size; exhaustion fails the run with one clear line instead of a
  watch-side panic. Host pointers are 64-bit, so the arena that plays here is bigger
  than the watch's 4 MB - use it to exercise the allocator, not to measure the budget.
- `env: {EMU_PSRAM_MB: "20"}` budgets the console's own allocations; refused asks reach
  the eviction hook and any step-down ladder the game has.

When the watch crashes with a heap-looking panic, reproduce it here first - the
**gyrotron-hw** skill owns the on-device half of that loop.

You changed anything under `tools/emu/`? Run one shot of a DIFFERENT game than the one
you were debugging - the emulator is shared infrastructure, and its regressions are
silent until someone else's suite fails.
