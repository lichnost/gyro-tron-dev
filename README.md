# gyro-tron-dev

An agent toolkit for the Gyro-Tron watch: an MCP server plus skills that carry a game
from an idea to an installable `.tat` package — design, C against `tat/tat_api.h`,
compile, screenshot-test in the deterministic emulator, package, validate, install to
a real watch over USB.

The `sdk/` directory is a self-sufficient snapshot of the toolchain: the API header and
its host, the engine and console sources the emulator compiles a game against, the
emulator, the packer and the on-device debug client, the reference game, and the
documentation. It mirrors the firmware repository's layout, so every tool's root
discovery works unchanged inside it.

## Install

As a ZCode plugin, straight from this repository:

```
zcode plugin add https://github.com/lichnost/gyro-tron-dev
```

(or add it through your marketplace/config of choice — the manifest is
`.zcode-plugin/plugin.json`.)

Then open a workspace that is — or contains — this clone, and try:

> Make me a simple Gyro-Tron game where you tilt a ball into a hole. Use the
> gyro-tron skills.

Expected: a new `games/<id>/`, compiling code, screenshots the agent has looked at, a
built `build/<id>.tat` that passes `validate_package`.

## Layout

| Path | What |
|---|---|
| `sdk/` | The exported toolchain snapshot (machine-written; see CONTRIBUTING) |
| `sdk/games/` | The reference game `echo` and every game an agent creates here |
| `sdk/docs/reference/GAME_API.md` | The game API — served to the agent as `tat://api/docs` |
| `mcp/` | The MCP server, its resources and the self-test |
| `skills/` | `gyrotron-core`, `gyrotron-build-test`, `gyrotron-ship`, `gyrotron-hw` |

## What the agent gets

**MCP tools** (server `gyro-tron-dev`, stdio): `create_game`, `list_games`,
`compile_game`, `screenshot` (deterministic headless scripts, golden-image
regressions), `build_package` (with a link check against the loader's frozen export
list), `validate_package`, `install_to_watch`, plus `get_log` and `watch_control` for
a real watch over USB.

**Resources:** `tat://api/header`, `tat://api/docs`, `tat://constraints`,
`tat://examples` and `tat://examples/<folder>`.

Game art (sprites, icons, backgrounds) is drawn by hand into `games/<id>/assets/` —
no image-generation tools ship with this plugin.

## Requirements

- Python 3 with Pillow (icon generation) and numpy (golden-image compare); `pyserial`
  is optional (port diagnostics). No other packages.
- A C++ compiler for the emulator (MSVC on Windows is found automatically, gcc/clang
  elsewhere).
- The xtensa toolchain (`~/.espressif` or `C:\Espressif\tools\xtensa-esp-elf`) only
  for `.tat` packaging; everything else works without it.
- Installing to a watch needs `gyrotron-manager` on PATH, a `dotnet publish` of
  `sdk/app/GyroTron.Manager`, or `dotnet`, plus the watch over USB.

## Working against the full firmware clone

If you develop inside a Gyro-Tron firmware checkout, the plugin uses the firmware tree
instead of `sdk/`: the repo root is found by walking up from the working directory
(marker: `tools/mktat.py` + `components/tat_api`), or set `GYROTRON_ROOT` in the MCP
server config to the clone's path. `sdk/` then simply goes unused.

## When a game hangs at the boot splash

A black screenshot with a healthy log line means the game task never started: the game
died between `tat_main` entry and its first loop iteration — most often a per-frame
allocation exhausted before the first draw, a stack overflow in the game task, or a
blocking call in the init path. Reproduce on the PC first (`compile_game` +
`screenshot` with the same game), then read the watch's raw UART via
`watch_control {serial: "", seconds: 30}` for the panic text, and resolve addresses
with `sdk/tools/emu/sym.py` against the firmware's map.

## How `sdk/` is updated

`sdk/` is machine-written from the firmware repository — see
[CONTRIBUTING.md](CONTRIBUTING.md).

MIT licensed — see [LICENSE](LICENSE).
