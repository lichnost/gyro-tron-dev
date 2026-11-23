---
name: gyrotron-ship
description: |
  Use when a Gyro-Tron game is done or nearly done - "pack it", "build the .tat",
  "install to the watch", "validate the package", "final check", "write the game README",
  or before handing a game to the user.
---

# Shipping a game

Run every step; report what passed and what could not be checked (no watch attached, for
instance) rather than skipping silently.

## Checklist

1. **Self-review the source** against `tat://constraints`: only `tat/tat_api.h` + libc;
   no per-frame allocation; KB state via `api->alloc()`; static screen drawn through dirty
   flags; pause menu + banner are the console's; bold text at scale >= 2; content inside
   the round; `keep_awake` true only mid-action; `id` matches game.json. The frozen-export
   part of this is now automatic - `build_package` runs `nm` on the linked image and fails
   the pack on any call outside `components/loader/exports.c` (before that check existed,
   a leak could only be discovered by a watch refusing to load) - but the machine has no
   xtensa toolchain, that check does not run either, so the manual read still matters.
   The doc at `tat://api/docs` is a design spec and names members the header does not
   ship (the drift is flagged at the top of that resource) - the header is the truth.
2. **Deterministic screenshots** (see **gyrotron-build-test**) of every distinct screen:
   start, playing, paused, game over. A game whose game-over shot you have never seen is
   not done - and once seen, make them golden regressions (`golden: true`), so the next
   change that breaks a screen fails loudly instead of waiting for an eyeball.
3. `build_package(game_id)` - packs with the repo's `mktat.py`, checks the frozen export
   list, and validates. It needs the xtensa toolchain; if the machine lacks it, say so and
   leave the `.tat` unbuilt rather than claiming success.
4. `validate_package(<path>)` - magic, CRCs, api version, icon. Must be `ok: true`.
5. `install_to_watch(<path>)` - optional, only when the user has a watch attached.
   On failure report the tool's hint and diagnostics (manager found or not, serial ports
   seen - usually: plug in the watch, run `gyrotron-manager list`). With the watch
   attached, close the chain from the **gyrotron-hw** skill: launch by id, a screenshot,
   `get_log` - and report what only that run could answer.
6. **Flip the status**: a finished game is `"production"` in game.json (drop the field or
   set it explicitly). `"dev"` keeps a game out of bulk build/install sweeps, so shipping
   while still dev means the next "install the new games" pass silently skips it.
7. **Report**: what the game does, controls, the screenshot paths, package path + size,
   validation result, install status, and anything known not to work - notably what only
   hardware can answer (IMU sign conventions against the emulator, yaw drift, real fps
   under worst-case load, gesture thresholds). If the game folder should stay in the
   repo, note the new files under `games/<id>/`.

## If the game ships to others

A `shot.png` (233x233 PNG) in the game folder is picked up by `mktat.py` as the package
preview the manager app shows before installing. Worth adding for anything shared.
