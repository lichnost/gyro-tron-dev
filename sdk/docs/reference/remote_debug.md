# Remote debugging — driving a real watch like the emulator

**Status:** reference (as-built). The authority for the wire formats is `components/net/ota_server.c`, `components/link/link.h` and `main/remote_debug.cpp`; the client is `tools/hw_debug.py`.

The emulator answers scripts and takes deterministic screenshots (`tools/emu/emu.py shot`). This is the same loop against real hardware: the watch runs the same script words, takes screenshots in fast formats, and offers it all over **Wi-Fi** or **USB**. A test written once runs on either — the emulator for determinism, the watch for the truth.

## Endpoints (Wi-Fi, port 80, the device key as usual)

| Endpoint | What it does |
|---|---|
| `GET /screen?fmt=png` | The browser picture, as before. ~3 s: most of it the PNG encoding. |
| `GET /screen?fmt=raw` | The whole frame as RGB565, little-endian — 434,312 bytes, no encoding, a second or two over a good link. |
| `GET /screen?fmt=delta` | Only what changed since the previous delta (GTD1 below). A static screen is a few dozen bytes; the first delta is the whole frame. Falls back to raw when PSRAM is too fragmented or a frame would not compress. |
| `GET /apps` | The carousel as JSON, `[{"i":1,"id":"breakout","name":"BREAKOUT"},...]`, in the order `/input?app=N` means it. |
| `POST /script` | Run script steps on the watch (below). The body is the script; the answer is a stream of records as they happen. |
| `GET /input?...` | The single-step remote control, unchanged. |
| `GET /log` | The recent log — how a test reads what the watch said. |

## The script language

Steps separated by `;` or newlines; seconds everywhere, like the emulator. `tap`, `hold`,
`swipe`, `btn`, `tilt`, `app N`, `home`, `sleep`, `wake` drive the console through the same
input path `/input` uses. Every input step settles ~150 ms so the following shot shows the
frame that came after the touch.

- `wait <seconds>`
- `tap <x> <y>` — a finger down and up (screen pixels, 466×466)
- `hold <x> <y> <ms>`
- `swipe <x0> <y0> <x1> <y1> [seconds]`
- `btn a|b [seconds]`
- `tilt <ax> <ay> <az>` / `tiltoff` — the injected sensor, as `Engine::injectTilt`
- `app <N>` / `home` — the carousel, per `GET /apps` (games first, then CLOCK and SETTINGS)
- `shot` — capture a frame, emit it
- `waitpx <x> <y> <#hex> [tol] [seconds]` — poll the live framebuffer until the pixel matches within `tol` per channel (default 8, timeout 10 s); fails the script on timeout

`POST /script` streams `[u8 type][u32 len][data]` records: type 1 is a screen frame (RGB565,
little-endian), 2 a text note (one per step, with timings), 3 an error, 0 the final ok/failed.
An unknown step, a failed step or a timed-out `waitpx` ends the script with the error.

The words `turn` / `pitch` / `yaw` from the emulator are deliberately absent: on a real watch
tilt comes from `tilt`.

## USB link commands

The same machinery without a network (`components/link/link.h`): `SHOT` 0x20 (fresh capture,
reply = u16 width, u16 height, u32 total), `SHOT_DATA` 0x21 (offset u32, reply up to 4 KB of
the cached frame), `INPUT` 0x22 (the `/input` query string as the payload), `APPS` 0x23 (the
carousel as entries of index/id/name). The screen commands are raw RGB565 chunks — there is
no delta over USB, a full pull is already only a second or two.

## GTD1 delta format

```
'G','T','D','1' | u16 width | u16 height | u16 rects
then per rect: u16 y | u16 rows | u8 enc | u8 pad | pixels
  enc 0 (raw): rows * width RGB565 pixels
  enc 1 (rle): (u16 run, u16 pixel) pairs, runs summing to rows * width
```

All little-endian. Rects cover the rows that changed since the previous delta, gaps of a few
clean rows folded in (a header costs more than a row of pixels); RLE is chosen per rect only
when it pays. The watch keeps one previous frame in PSRAM (434 KB, allocated on the first
delta request and kept); the client keeps its own copy.

## The client

`tools/hw_debug.py` — `--host` picks Wi-Fi (the key is borrowed over USB if not given),
`--port` picks USB. `--apps`, `--game <id>`, `--shot file.png`, `--script "..."`, `--log`.
Script shots land in `build/hw-debug/`. Over USB the script is interpreted by the client
step by step; over Wi-Fi it runs on the watch itself, so it stays in sync even on a weak
link. A USB swipe always runs 160 ms (the link has no long-swipe primitive); Wi-Fi scripts
take the full duration.

On-device, `main/remote_debug.cpp` does the capturing and the interpreting; `main.cpp` wires
it to the engine, the app table and the console's own `applyControl`, so HTTP, USB and
scripts share one set of input words.

### Lifecycle and diagnostics

`/input` and the USB `INPUT` command also take `rescan=1` (the launcher re-reads the games
folder on the home screen) and `reboot=1` (restart ~300 ms after answering). The client adds:

| Option or word | What it does |
|---|---|
| `app <id>` | Script word; the id is resolved against the carousel at that moment. |
| `waitlog "<regex>" [seconds]` | Script word; polls the log for a new matching line (default 30 s). Client side on both transports. |
| `--install FILE.tat` | USB. Runs the manager CLI, then checks the size of `Games/<file>`. |
| `--rescan`, `--reboot` | The two control words; `--reboot` runs last. |
| `--game ID --retry N` | Re-resolves the carousel on each attempt. |
| `--serial [QUERY] --seconds N` | USB. Sends one input, then prints raw UART, reopening the port if a reset drops it. |
| `--doctor [--host H]` | Interface, name, ARP, TCP, HTTP: says which leg is broken. ARP without TCP points at client isolation or a firewall. |
