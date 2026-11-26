# gyro-tron-dev — notes for agents

The standalone Gyro-Tron game-development toolkit: an MCP server (`mcp/server.py`)
plus skills, working against `sdk/` — an exported snapshot of the watch's toolchain
(the API header and host, the engine and console sources the emulator compiles, the
emulator, the packer, the debug client, the reference game `echo` and the docs).

## Layout rules

- `sdk/` is machine-written and **must not be edited by hand** — not by a human and
  not by an agent. It is exported from the firmware repository; a hand edit is
  silently overwritten by the next export and hides the real fix. If something in
  `sdk/` looks wrong, say so in the report instead of patching it.
- `mcp/` and `skills/` are the editable code. The self-test is the acceptance gate:
  `python mcp/selftest.py` after any change there.
- Everything disposable goes under `sdk/build/` (gitignored): emulator runs,
  screenshots, packed `.tat` files, a scratch game being debugged, throwaway scripts in
  their own `sdk/build/<name>/` directory. Games created by agents live in
  `sdk/games/<id>/` — that tree is writable by design. The root is `sdk/`, so the
  `games/<id>` and `build/<id>.tat` paths the tools take and return are inside it.

## What you can run here

- `python mcp/selftest.py` — golden compare, export-list parse, doc-drift check, MCP
  protocol over stdio. Needs Pillow + numpy in the active Python.
- The emulator against any game in `sdk/games/`:
  `python sdk/tools/emu/emu.py play <game>`, `shot <game> "<script>"`, `sheet` —
  see `sdk/tools/emu/README.md`. Console screens work too (`@boot`, `@power`,
  `@hold`, `@home`).
- `python sdk/tools/mktat.py sdk/games/<id>` packs `sdk/build/<id>.tat` (xtensa
  toolchain needed); `validate_package` checks a package without one.
- `python sdk/tools/hw_debug.py --port COM4 ...` drives a real watch; needs pyserial.
- `install_to_watch` runs the manager app from source through `dotnet run`. The app is
  a GUI binary and prints nothing, so the tool reports only success or failure: confirm
  the install in `get_log` (`link: stored /data/Games/<id>.tat`), never by the empty
  output alone.
- There is no hosted CI here: run the self-test yourself before reporting a change to
  `mcp/` or `skills/` as done.

MSVC on a Russian-locale Windows emits non-UTF-8 diagnostics — decode compiler output
with `errors='replace'` in any new tooling that captures it (the emulator already
does).

## Conventions

Documentation and comments describe functionality as it exists now — no history, no
"was changed from X to Y". They must not cite plans or their sections either: a plan is
a backlog or a kept record, not as-built documentation, and citing it is citing history
by another name — describe the functionality itself. Comments say *why*, not *what*.
4 spaces, 120 columns, braces on the same line. The selftest passing is the definition
of "done" for `mcp/` changes; a compile plus a looked-at screenshot is the definition
of "done" for a game.
