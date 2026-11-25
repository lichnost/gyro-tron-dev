#!/usr/bin/env python3
"""Pack a game into a .tat - the single file people install, trade and share.

    python tools/mktat.py games/<id>            (-> build/<id>.tat)

A game directory holds its source (one or more .c files), optionally an assets/ folder,
and a game.json naming it. Everything that ends up in the package comes from there, so a
package is exactly what the author wrote and nothing of this repo's build.

A GB ROM-cartridge descriptor (games/gb_*, "type": "gb" in game.json) is the exception:
it carries no source at all. Its code is the template game's (the emulator in kits/gbboy/)
and its ROM is the game, fetched and cached by kits/gbboy/tools/gbmake.py, which this
tool routes to. The .tat that comes out is an ordinary package either way.

The interesting part is how the code is linked. See docs/reference/GAME_API.md section 6.1: the game
is linked as ONE contiguous image at base 0 with relaxation off and the relocations kept.
That leaves the loader a single relocation type to handle, because everything PC-relative
inside the image is already correct and every call out of it went through the literal pool
as a plain 32-bit word.
"""
import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# the shared module lives beside the backend tools here and beside this file in the
# exported SDK (sdk/tools/) - take whichever of the two exists
_backend_tools = os.path.join(ROOT, "backend", "tools")
sys.path.insert(0, _backend_tools if os.path.isdir(_backend_tools) else HERE)
import tatpkg  # noqa: E402  the container's shared parse/sign module

MAGIC = b"TATPKG\0\x01"
KIND_GAME = 1   # the kind byte is part of the frozen header; values 2+ are unassigned

# Section types, four bytes each so a dump is readable.
SEC_CODE = b"CODE"
SEC_ICON = b"ICON"
ICON_PX = 210            # what tools/make_icons.py draws, and the carousel's circle
ICON_MAX_PX = 232        # the largest the launcher will decode
ICON_MAX_BYTES = 4096    # LINK_MAX_PAYLOAD: one frame over USB
SEC_ASSET = b"ASST"

# The linker script that makes the loader's job small. Written out next to the build so a
# package can be reproduced from the tool alone.
LINKER_SCRIPT = """\
/* One contiguous image at zero: literals, code, read-only data, data, then bss. Every
 * distance inside the image is what the linker assumed, so the loader can drop it
 * anywhere and only the absolute references need fixing.
 *
 * The literal pool gets its own output section, and that is not cosmetic. --emit-relocs
 * writes each addend relative to the INPUT section, so an output section only relocates
 * correctly when it begins with the input section the addends are counted from. With
 * literals and code sharing one .text, every function pointer came out short by the size
 * of the literal pool - and pointed at the middle of some other function.
 *
 * Literals still have to sit at a LOWER address than the code that reads them, because
 * l32r only reaches backwards, so the order stays: literals, then text. */
ENTRY(tat_game)
SECTIONS
{
  . = 0;
  .literal : ALIGN(4) { *(.literal .literal.*) }
  .text : ALIGN(4) { *(.text .text.*) }
  .rodata : ALIGN(4) { *(.rodata .rodata.*) *(.srodata .srodata.*) }
  .data : ALIGN(4) { *(.data .data.*) *(.sdata .sdata.*) }
  .bss (NOLOAD) : ALIGN(4) { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.xtensa.info) *(.xt.prop*) *(.xt.lit*) *(.debug*) }
}
"""


def find_toolchain(gcc=None):
    """The same compiler the firmware is built with, wherever ESP-IDF put it."""
    if gcc:
        # The firmware build knows exactly which compiler it is using, and says so.
        return gcc, gcc[: gcc.rfind("gcc")] + "ld" + gcc[gcc.rfind("gcc") + 3 :]
    pats = [
        r"C:\Espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc*",
        os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"),
    ]
    for pat in pats:
        hits = sorted(glob.glob(pat))
        if hits:
            gcc = hits[-1]
            # Swap the gcc suffix for ld at the executable name, not the first "gcc" in
            # the path (a directory can be called that too).
            return gcc, gcc[: gcc.rfind("gcc")] + "ld" + gcc[gcc.rfind("gcc") + 3 :]
    sys.exit("could not find the xtensa toolchain; run this from an ESP-IDF install")


def build_code(game_dir, work, verbose=False, gcc=None):
    """Compile every .c in the game directory and link it into one relocatable image."""
    gcc, ld = find_toolchain(gcc)
    os.makedirs(work, exist_ok=True)

    # Recursive: a game may keep sources in its own subfolders (e.g. a vendored
    # emulator core under games/<id>/). The package is still exactly the
    # game directory - nothing of this repo's build joins it.
    sources = sorted(glob.glob(os.path.join(game_dir, "**", "*.c"), recursive=True))
    # A _builtin.c exists only to hand the firmware build its embedded assets. A package
    # carries the files themselves, so it must not be compiled in.
    sources = [s for s in sources if not s.endswith("_builtin.c")]
    if not sources:
        sys.exit(f"no .c files in {game_dir}")

    objs = []
    for src in sources:
        obj = os.path.join(work, os.path.basename(src)[:-2] + ".o")
        # -fno-merge-constants keeps string literals out of .rodata.str1.1. That section is
        # SHF_MERGE, and the addends ld writes for a merged section under --emit-relocs do
        # not mean what they do everywhere else - they came out negative, pointing below
        # the section, and every string in the game arrived as rubbish while every number
        # beside it was perfect. Ordinary sections relocate the ordinary way.
        # No -ffunction-sections here, deliberately. With one section per function, every
        # function pointer in the game's descriptor came out of the link as ".text + 0":
        # --emit-relocs does not fold an input section's offset within the output section
        # into the addend, and with a section per function that offset is the whole answer.
        # One .text per file keeps the addends meaning what they say. It costs a little
        # size, since there is no --gc-sections to drop what is unused, and a game that is
        # 19 KB does not care.
        cmd = [gcc, "-c", "-std=gnu17", "-O2", "-mlongcalls", "-fno-merge-constants",
               "-I", os.path.join(ROOT, "components/tat_api/include"),
               "-o", obj, src]
        # Same rule as the emulator: a game's own include/ (vendored engine, shims) goes
        # on the path for angle-bracket includes.
        game_inc = os.path.join(game_dir, "include")
        if os.path.isdir(game_inc):
            cmd[3:3] = ["-I", game_inc]
        # And a forced prelude (include/cd_preload.h) may remap libc names the watch's
        # frozen export list does not carry - malloc, stdio, double maths.
        preload = os.path.join(game_inc, "cd_preload.h")
        if os.path.isfile(preload):
            cmd += ["-include", preload]
            # A ported engine keeps its double literals; the watch has no double maths,
            # so unsuffixed literals collapse to float along with the keyword itself.
            cmd += ["-fsingle-precision-constant", "-fno-builtin"]
        if verbose:
            print("  " + " ".join(cmd))
        subprocess.run(cmd, check=True)
        objs.append(obj)

    # Merge everything into one object first. Same reason the literal pool is kept separate
    # below: an addend is relative to its input section, so only the input section that
    # lands at the start of an output section relocates correctly. With one object there is
    # one input .text, one .rodata and one .bss, and each of them starts its own output
    # section - which keeps a game of several source files as correct as a game of one.
    if len(objs) > 1:
        merged = os.path.join(work, "all.o")
        cmd = [ld, "-r", "-o", merged] + objs
        if verbose:
            print("  " + " ".join(cmd))
        subprocess.run(cmd, check=True)
        objs = [merged]

    script = os.path.join(work, "game.ld")
    with open(script, "w") as f:
        f.write(LINKER_SCRIPT)

    elf = os.path.join(work, "game.elf")
    cmd = [ld, "-q", "--no-relax",
           "--unresolved-symbols=ignore-all", "--no-warn-rwx-segments",
           "-T", script, "-o", elf] + objs
    if verbose:
        print("  " + " ".join(cmd))
    subprocess.run(cmd, check=True)
    return elf


def frozen_exports():
    """The names a loaded game may reach by name - the loader's frozen table, every
    EXPORT() in components/loader/exports.c, compiler helpers included. Plus the one
    name a game may import that the console does NOT export: tat_assets, the asset
    table the loader fills from the package itself (components/loader/loader.c,
    apply_relocations special-cases it before consulting the export table)."""
    path = os.path.join(ROOT, "components", "loader", "exports.c")
    names = set(re.findall(r"EXPORT\((\w+)\)", open(path).read()))
    names.add("tat_assets")
    return names


def undefined_symbols(nm_output):
    """The names nm reports as undefined: lines like '                 U memcpy'."""
    return [ln.split()[-1] for ln in nm_output.splitlines() if ln.strip().startswith("U ")]


def check_unresolved(elf, gcc):
    """The frozen-C-library gate. The link above runs with --unresolved-symbols=ignore-all
    (a relocs-only image cannot resolve anything yet), so a call outside the frozen list
    walks out of here looking like any other symbol - and the watch refuses the package
    at load with 'symbol not exported'. The first game written fresh against the API
    reached for memmove and would not load; nothing on this PC would have said so, and
    the emulator could not either, because it is linked, not loaded. nm reads the same
    truth out of the ELF before it is ever packed."""
    nm = gcc[: gcc.rfind("gcc")] + "nm" + gcc[gcc.rfind("gcc") + 3:]
    r = subprocess.run([nm, "-u", elf], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"nm could not read {elf}: {r.stderr.strip()}")
    leak = sorted(s for s in undefined_symbols(r.stdout) if s not in frozen_exports())
    if leak:
        sys.exit("calls outside the frozen C library - the watch would refuse to load "
                 f"this package: {', '.join(leak)}. A game reaches only tat_api.h and "
                 "the exports in components/loader/exports.c; if something ordinary is "
                 "missing from that table, add it there (it is append-only by design)")


def section(kind, payload):
    """One section: a 4-byte type, then the bytes, with its own CRC in the table."""
    return kind, payload, zlib.crc32(payload) & 0xFFFFFFFF


def build_package(game_dir, out, verbose=False, gcc=None, work_root=None, skin=None):
    meta_path = os.path.join(game_dir, "game.json")
    if not os.path.exists(meta_path):
        sys.exit(f"{game_dir} has no game.json")
    with open(meta_path) as f:
        meta = json.load(f)

    for key in ("id", "name", "author", "version"):
        if key not in meta:
            sys.exit(f"game.json is missing {key!r}")
    if not meta["id"].replace("_", "").isalnum() or meta["id"] != meta["id"].lower():
        sys.exit("id must be lowercase letters, digits and underscores")

    # Maturity, not availability: a "dev" game packs fine, it is just excluded from bulk
    # build/install sweeps (only an explicit pack by name picks it up). Nothing about it
    # travels in the package - this is a repository-side flag, so a missing status means
    # production and old descriptors stay valid.
    status = meta.get("status", "production")
    if status not in ("dev", "production"):
        sys.exit(f'unknown game.json "status": {status!r} (the values are "dev" and "production")')

    # Which kind of thing this is. A plain game (no "type") is packed right here. A GB
    # ROM-cartridge descriptor - "type": "gb", or the same template+rom shape without the
    # type field - has no .c files to compile: the code is the
    # template game's and the ROM is the game, download and cache included, and that is
    # kits/gbboy/tools/gbmake.py's job. One .tat writer, one downloader, no second
    # opinion to drift.
    kind = meta.get("type", "")
    if kind and kind != "gb":
        sys.exit(f"unknown game.json \"type\": {kind!r} (the packer knows \"gb\"; a plain game needs no type)")
    if kind == "gb" or ("template" in meta and "rom" in meta):
        if skin:
            sys.exit("a skin replaces a game's art assets; a GB descriptor carries only its ROM")
        sys.path.insert(0, os.path.join(ROOT, "kits", "gbboy", "tools"))
        import gbmake  # noqa: E402  the GB kit owns the packer; tools/ stays base-only
        if not gbmake.load_descriptor(game_dir):
            sys.exit(f"{game_dir} is not a GB descriptor: its game.json needs \"template\" and \"rom\"")
        gbmake.build_one(game_dir, argparse.Namespace(template=None, refetch=False),
                         out=out, gcc=gcc)
        return

    work = os.path.join(work_root or os.path.join(ROOT, "build", "tat"), meta["id"])
    elf = build_code(game_dir, work, verbose, gcc)
    check_unresolved(elf, find_toolchain(gcc)[0])
    with open(elf, "rb") as f:
        code = f.read()

    sections = [section(SEC_CODE, code)]

    # Every game has an icon. It was optional once, and the result was a home screen and a
    # library full of identical blank cartridges that could only be told apart by reading.
    # The size limit is the link's: the watch sends an icon to the app in one frame.
    icon = os.path.join(game_dir, "icon.png")
    if not os.path.exists(icon):
        sys.exit(f"{game_dir} has no icon.png - every game needs one "
                 f"(a {ICON_PX} x {ICON_PX} PNG; docs/reference/GAME_API.md section 2)")
    with open(icon, "rb") as f:
        icon_png = f.read()
    if icon_png[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("icon.png is not a PNG")
    icon_w, icon_h = struct.unpack(">II", icon_png[16:24])
    if icon_w > ICON_MAX_PX or icon_h > ICON_MAX_PX:
        sys.exit(f"icon.png is {icon_w} x {icon_h}; the most the home screen will take is "
                 f"{ICON_MAX_PX} x {ICON_MAX_PX}, and {ICON_PX} x {ICON_PX} is what it is drawn at")
    if len(icon_png) > ICON_MAX_BYTES:
        sys.exit(f"icon.png is {len(icon_png)} bytes; the limit is {ICON_MAX_BYTES}. "
                 "Pixel art with few colours, saved as an indexed PNG, fits easily")
    sections.append(section(SEC_ICON, icon_png))

    # Assets travel by the name the game asks for, so the loader can answer api->asset()
    # without knowing anything about what is in them.
    # Normally a game keeps its files in its own assets/. The built-in games share one art
    # folder with the firmware build, so game.json may point at it rather than the files
    # being copied into the repo twice.
    asset_dir = os.path.join(game_dir, meta.get("assets", "assets"))
    # A skin is a folder of files with the same names as some of the game's assets, which
    # take their place in this one package. It is how somebody puts their own art on a game
    # - their own ships in STARFALL, say - without touching its source, and the folder can
    # be anywhere: art that is theirs to use but not this repository's to publish stays out
    # of it. A skin cannot add files the game does not already ask for.
    skinned = {}
    if skin:
        for path in glob.glob(os.path.join(skin, "*")):
            skinned[os.path.basename(path)] = path
    for path in sorted(glob.glob(os.path.join(asset_dir, "*"))):
        name = os.path.basename(path)
        if name in skinned:
            path = skinned.pop(name)
            print(f"  skin: {name} from {path}")
        if len(name.encode()) > 15:
            sys.exit(f"asset name too long (15 bytes max): {name}")
        with open(path, "rb") as f:
            body = f.read()
        sections.append(section(SEC_ASSET, name.encode().ljust(16, b"\0") + body))
    skinned.pop("icon.png", None)
    if skinned:
        sys.exit("the skin has files this game does not use: " + ", ".join(sorted(skinned)))

    # Header, then the section table, then the payloads. Offsets are from the file start,
    # so the watch can check a section's CRC without having parsed anything before it.
    # 96 bytes of header, then the section table. Getting this wrong by four silently
    # shifts every section offset and makes total_size disagree with the file, which the
    # watch rejects with no clue as to why - so it is spelled out rather than counted.
    HEADER_BYTES = 8 + 4 + 4 + 1 + 1 + 2 + 2 + 2 + 16 + 24 + 24 + 4 + 2 + 2
    assert HEADER_BYTES == 96, HEADER_BYTES
    head_len = HEADER_BYTES + 16 * len(sections)
    blobs, table, off = [], b"", head_len
    for kind, payload, crc in sections:
        table += struct.pack("<4sIII", kind, off, len(payload), crc)
        blobs.append(payload)
        off += len(payload)
    total = off

    def fixed(s, n):
        b = s.encode()
        if len(b) >= n:
            sys.exit(f"{s!r} is too long (max {n - 1} bytes)")
        return b.ljust(n, b"\0")

    body = struct.pack("<BBHHH", KIND_GAME, 0,
                       meta.get("api_major", 1), meta.get("api_minor", 0),
                       int(meta["version"]))
    body += fixed(meta["id"], 16) + fixed(meta["name"], 24) + fixed(meta["author"], 24)
    body += struct.pack("<BBBB", *(list(meta.get("accent", [255, 255, 255])) + [0]))
    body += struct.pack("<H", len(sections)) + b"\0\0"
    # Everything from `kind` to the end of the fixed header: offsets 16..95.
    assert len(body) == HEADER_BYTES - 16, len(body)

    # header_crc32 covers everything after itself, so a truncated or shuffled file is
    # caught before a single byte of it is trusted.
    after = struct.pack("<I", total) + body + table
    header = MAGIC + struct.pack("<I", zlib.crc32(after) & 0xFFFFFFFF) + after

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(header)
        for b in blobs:
            f.write(b)

    print(f"{out}  {total / 1024:.1f} KB")
    print(f"  {meta['name']} ({meta['id']}) v{meta['version']} by {meta['author']}"
          + (" - in development" if status == "dev" else ""))
    for kind, payload, _ in sections:
        label = kind.decode().strip()
        if kind == SEC_ASSET:
            label += " " + payload[:16].rstrip(b"\0").decode()
        print(f"  {label:<12} {len(payload) / 1024:7.1f} KB")


def sign_in_place(out, key_path):
    """Append the SIG section to the freshly written package. The key file is the
    signing seed (hex, one line - backend/tools/tatpkg.py genkey writes it); the private half
    of the official channel's key lives in CI variables, never in this repo."""
    with open(out, "rb") as f:
        data = f.read()
    signed = tatpkg.sign_package(data, tatpkg.read_seed(key_path))
    with open(out, "wb") as f:
        f.write(signed)
    state, detail = tatpkg.verify_signature(signed, keys=tatpkg.trusted_keys())
    print(f"  signed ({detail})")
    if state != tatpkg.SIG_VALID:
        sys.exit(f"the fresh signature does not verify: {detail}")


def main():
    ap = argparse.ArgumentParser(description="pack a game into a .tat")
    ap.add_argument("game_dir", help="a directory with game.json, .c sources and assets/")
    ap.add_argument("--out", help="where to write the package")
    ap.add_argument("-v", "--verbose", action="store_true", help="show the build commands")
    ap.add_argument("--gcc", help="the xtensa gcc to use (the firmware build passes its own)")
    ap.add_argument("--work", help="where intermediate files go (default build/tat)")
    ap.add_argument("--skin", help="a folder of files that replace the game's assets of the same name")
    ap.add_argument("--sign", metavar="KEYFILE",
                    help="append a SIG section with this Ed25519 seed file (the official channel's key)")
    args = ap.parse_args()

    out = args.out or os.path.join(ROOT, "build", os.path.basename(args.game_dir.rstrip("/\\")) + ".tat")
    build_package(args.game_dir, out, args.verbose, args.gcc, args.work, args.skin)
    if args.sign:
        sign_in_place(out, args.sign)


if __name__ == "__main__":
    main()
