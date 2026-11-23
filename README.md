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

Clone this repository. The server needs nothing but the clone:

```bash
git clone https://github.com/lichnost/gyro-tron-dev.git
```

Add the MCP server to your agent's configuration — command `python`, argument the
path to `mcp/server.py` inside the clone:

```json
{
  "mcpServers": {
    "gyro-tron-dev": { "command": "python", "args": ["<clone>/mcp/server.py"] }
  }
}
```

The server finds `sdk/` on its own, wherever the agent's working directory is: a root
is looked for by walking up from the working directory (marker: `tools/mktat.py` plus
`components/tat_api/include/tat/tat_api.h`), and when none is found the clone's own
`sdk/` is used. To force a particular root, set `GYROTRON_ROOT`.

The skills in `skills/` are plain `SKILL.md` files with no host-specific parts; give
them to the agent the way it takes skills. The repository also carries a ZCode plugin
manifest (`.zcode-plugin/plugin.json`) that declares the same server and the skills.

## Trial

In any workspace, ask:

> Make me a simple Gyro-Tron game where you tilt a ball into a hole. Use the
> gyro-tron skills.

Expected: a new `sdk/games/<id>/`, compiling code, screenshots the agent has looked at,
a built `sdk/build/<id>.tat` that passes `validate_package`.

## Layout

| Path | What |
|---|---|
| `sdk/` | The exported toolchain snapshot (machine-written; see CONTRIBUTING) |
| `sdk/games/` | The reference game `echo` and every game an agent creates here |
| `sdk/build/` | Emulator runs, screenshots and packed `.tat` files (gitignored) |
| `sdk/docs/reference/GAME_API.md` | The game API — served to the agent as `tat://api/docs` |
| `mcp/` | The MCP server, its resources and the self-test |
| `skills/` | `gyrotron-core`, `gyrotron-build-test`, `gyrotron-ship`, `gyrotron-hw` |

Every path a tool takes or returns is relative to the root, and the root is `sdk/`:
`create_game` writes `games/<id>/`, `build_package` writes `build/<id>.tat`. A relative
`path` given to `validate_package` or `install_to_watch` is resolved against the same
root, not against the agent's working directory.

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
- Installing to a watch needs the watch over USB and one of: `dotnet` (the manager
  app in `sdk/app/GyroTron.Manager` is run from source), `gyrotron-manager` on PATH,
  or a `publish/` build of it in `sdk/app/GyroTron.Manager`.

The manager app is a GUI binary and prints nothing on its command line, so an install
done through `dotnet run` reports only success or failure. What actually reached the
watch is in its log: `get_log` shows `link: stored /data/Games/<id>.tat` for a package
the console accepted.

## Working against the full firmware clone

Inside a Gyro-Tron firmware checkout the plugin uses the firmware tree instead of
`sdk/`: the walk up from the working directory finds that root first, and `sdk/` goes
unused. With both trees at hand, `GYROTRON_ROOT` picks the one to work on.

## When a game hangs at the boot splash

A black screenshot while the watch still answers — the log, the status, injected
input all work — means the game task never started, not that the game is slow. The
game task needs a contiguous block of internal RAM for its stack, the same pool Wi-Fi,
the HTTP server and the USB driver allocate from; a boot whose allocations left no such
block has nothing that draws, and the screen stays at the splash's last frame. Which
boot hits it depends on that boot's allocations, so it comes and goes.

The firmware checks the task creation, retries, and on final failure logs the largest
free internal block and restarts; a console that fails several boots in a row comes up
in safe mode. So when this is seen, read the log with `get_log` first — it tells a
refused package, a failed task creation and a counted bad boot apart — and
`watch_control {reboot: true}` starts a clean boot.

## How `sdk/` is updated

`sdk/` is machine-written from the firmware repository — see
[CONTRIBUTING.md](CONTRIBUTING.md).

MIT licensed — see [LICENSE](LICENSE).
