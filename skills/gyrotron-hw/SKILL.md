---
name: gyrotron-hw
description: |
  Use when testing or debugging a Gyro-Tron game on a real watch over USB - "install
  and run it on the watch", "what is the watch saying", "it crashed on hardware",
  "read the watch's log", a hang or a panic on the device, or verifying a shipped
  package end to end. The emulator-side loop is the gyrotron-build-test skill.
---

# On the real watch

The emulator is pixel-exact but it cannot feel tilt, measure real fps, or catch a
loader/allocator/memory problem that only exists on the watch. This loop closes that
gap. The watch talks USB here; every tool below drives `tools/hw_debug.py` - there is
exactly one driver, and no step of this loop needs Python or PowerShell detours.

## The chain

1. `install_to_watch(<path>)` - validate + install via the manager. Use
   `watch_control {game: "<id>"}` right after: the launch re-resolves the carousel by
   id at that moment (never trust an app number remembered from earlier - reinstalls
   reorder it).
2. `watch_control {script: "..."}` - the same script words as the emulator
   (`tap/hold/swipe/btn/tilt`, `wait`, `waitpx`, `shot`), plus two client-side words:
   `app <id>` and `waitlog "<regex>" [s]` (sync on a boot stage or a game log line
   instead of a fixed `wait 16`).
3. `get_log` - the fastest answer to "what is the watch saying". Read it before
   guessing: an alloc failure, a loader refusal, a game's own trace are all there.
4. `watch_control {reboot: true}` - restart without esptool's RTS dance.
5. `watch_control {rescan: true}` - after an out-of-band install, makes the launcher
   re-read the games folder on the home screen (needs firmware with the `rescan=1`
   control word; older firmware needs a reboot instead).

Note which steps the firmware actually supports: `rescan`/`reboot` and the script
words beyond the emulator's set need a recent firmware. A `bad request` from the watch
means this build predates the word - say so, do not retry.

## When it crashes or hangs

- `watch_control {serial: "", seconds: 30}` sends nothing and prints the raw UART:
  ESP-IDF panic text and backtraces included. A panic resets the chip and drops the
  USB port; the tool reopens it and keeps listening. Resolve addresses with
  `tools/emu/sym.py` against the firmware's map.
- A black screenshot with a healthy log means the game task never started - see the README section
  `When a game hangs at the boot splash`.
- The heap shim's bugs show only on the watch: build the game in the emulator with
  `EMU_HEAP_MB` set (see **gyrotron-build-test**) to reproduce memory pressure on the PC.

## Wi-Fi instead of USB

The same tools and script words run over Wi-Fi (`--host`); the plugin's tools are
USB-first because that always works. If a Wi-Fi path refuses to connect at all, run
`python tools/hw_debug.py --doctor [--host <ip>]` by hand - it says which leg is
broken (interface, name, ARP, TCP, HTTP; ARP without TCP points at client isolation
or a firewall). Do not retry a broken leg in a loop.

## Report

What ran, the watch's firmware version from the hello line, log excerpts that matter,
screenshot paths, and what only a human can judge (tilt feel, real fps). If the watch's
firmware lacks a word the loop needed, that goes in the report too - it is a flashing
task for the user, never something to do unasked (flashing overwrites a live device).
