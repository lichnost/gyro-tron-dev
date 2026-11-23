# Contributing to gyro-tron-dev

MIT licensed. By contributing you agree your work goes out under the same licence.

## The one rule about `sdk/`

`sdk/` is **machine-written**: it is exported from the Gyro-Tron firmware repository by
its export script, and that script is its only writer. PRs against anything under
`sdk/` are closed with a pointer to the firmware repository — fix it there and the next
export carries it. A `sdk/SDK_VERSION` file pins the firmware commit each snapshot was
taken from; an issue report should quote it.

Everything else takes outside PRs:

- `mcp/` — the MCP server, its resources, and `selftest.py`.
- `skills/` — the four SKILL.md files.
- The docs at the repo root.

## The acceptance gate

    python mcp/selftest.py

Golden-image compare, the frozen export-list parse, the doc-vs-header drift check and
the MCP protocol over stdio. It must pass before any change to `mcp/` or `skills/` is
merged; CI runs the same, plus a compile-and-screenshot of `sdk/games/echo`, on Windows
and Linux.

## House style

The existing code is the specification. Comments say *why*, not *what*. Plain names,
4 spaces, 120 columns, braces on the same line. Documentation describes functionality
as it exists now — no change history in docs or comments; that is what commits are for.

## Scratch artifacts

Anything disposable — emulator runs, screenshots, packed packages, a game being
debugged — lives under `build/` (gitignored). The emulator and packer already write
there; keep throwaway scripts in `build/<name>/` too.
