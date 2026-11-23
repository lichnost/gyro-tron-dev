---
name: gyrotron-core
description: |
  Use when creating, designing or editing a Gyro-Tron game (.tat package) - e.g.
  "make a game for the watch", "add a level to pindrop", "port this idea to
  Gyro-Tron", or any task mentioning tat_api.h, games/<id> or .tat files. Covers the
  end-to-end workflow and the hard rules of the platform.
---

# Gyro-Tron game development

You develop games for Gyro-Tron, a round-screen watch (466x466 AMOLED, touch, tilt).
Games are plain C against one header, `tat/tat_api.h`, packed into installable `.tat`
files. The MCP server `gyro-tron-dev` gives you the toolchain; use it instead of
running the python tools by hand.

## The workflow

1. **Design first.** Before code, agree with the user on: the core loop in one sentence,
   the controls (tilt and/or touch; what swipe left and PWR do), win/lose, the visual
   idea for a round dark screen. Sketch what each screen shows. This is the one place to
   loop with the user - after it, you drive the whole cycle yourself.
2. `create_game(id, name, author, accent)` - a working tilt-game skeleton appears in
   `games/<id>/`: `game.json`, `<id>.c` (compiles, runs, demonstrates tilt + tap + pause
   menu + saves + banner), placeholder icon. The skeleton is `"status": "dev"` in
   game.json: dev games stay out of bulk build/install sweeps (only an explicit pack or
   install by id touches them); the ship checklist flips it to `"production"` when done.
3. Read `tat://api/header` and `tat://constraints` (resources), and one example game via
   `tat://examples/<folder>` - `echo` is the reference.
4. Write the game into `<id>.c`. Edit the template; do not start from an empty file.
5. Iterate autonomously - the **gyrotron-build-test** skill owns this loop:
   `compile_game` -> `screenshot` (script) -> look at the PNGs -> fix. Repeat.
6. Polish: tones for actions, the banner for GAME OVER / LEVEL N, saves for best scores,
   `keep_awake` only while the player is mid-action. Art (sprites, the game icon) is
   drawn by hand - this plugin ships no image-generation tools.
7. Ship - the **gyrotron-ship** skill owns the end: `build_package`, `validate_package`,
   optional `install_to_watch` and the on-device chain (`gyrotron-hw`) when a watch is
   attached, then a short report.

## Hard rules (the full list is in tat://constraints)

- `#include "tat/tat_api.h"` and libc only. Plain C, no C++. File-scope `const` must be a
  constant expression.
- `begin()` stores the `tat_api_t*`; `update(dt)` then `draw()` every frame; nothing
  allocates per frame; KB-sized state goes through `api->alloc()`, never statics.
- Round screen: keep content inside the circle; canvas is 233x233 palette at scale 2.
- Tap is primary, PWR secondary, swipe left opens the console pause menu, BOOT is home
  (never yours). Pause menu and banner are the console's widgets - use them.
- Screen is static between state changes: dirty flags, not animation loops.
- `id` (lowercase, max 15) is the save namespace - pick it once, pick it right.
- Only the console's frozen C-library list links on the watch - float maths, string and
  memory functions, snprintf, abs/labs. Anything else (qsort, sscanf, ctype, printf,
  double maths like pow, rand) passes the emulator and the packer without a warning and
  then fails to load on the watch. The list is in tat://constraints.
- Set `api_major/api_minor` from the header's `TAT_API_MAJOR/MINOR`.

## House style

4 spaces, 120 columns, braces on the same line, plain names. Comments say *why*, not
what. Everything you write - code, comments, commit subjects - is English.
