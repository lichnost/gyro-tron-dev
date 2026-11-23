#!/usr/bin/env python3
"""Resolve an address to a symbol using an emulator build's linker map.

The emulator's build step writes the link map next to the library
(build/emu/<game>/<game>[+DEFINE].dll.map), so a crash report's address - or the
RVA a Windows fault handler prints - can be named instead of guessed:

    python tools/emu/sym.py build/emu/<game>/<game>.dll.map 0x180012A40
    python tools/emu/sym.py <game> 12A40 --rva        # a game id works too
    python tools/emu/sym.py <game> 0x7FF9ABCD1234 --base 0x7FF9ABCD0000

Takes the linker map of either toolchain: MSVC (/MAP, whose "Rva+Base" column
already carries the preferred image base, 0x180000000 for a 64-bit DLL) or gcc
(-Wl,-Map, whose addresses are the link-time VMAs). A fault address from a
loaded DLL equals preferred-base + RVA only if the loader put it there; pass
--base with the actual load address and the query is rebased onto the map's own
base. The nearest symbol at or below the address is the answer either way.
"""
import argparse
import os
import re
import sys

# The preferred base of a 64-bit MSVC DLL image, and the floor of its RVAs.
DEFAULT_BASE = 0x180000000


def parse_map(text):
    """Return a sorted list of (address, name) from an MSVC or gcc map."""
    syms = []
    # MSVC "Publics by Value": 0001:00000100  name  180001010 f foo.obj
    for m in re.finditer(r"^\s*[\dA-Fa-f]{4}:[\dA-Fa-f]{8}\s+(\S+)\s+([\dA-Fa-f]{9,16})\s", text, re.M):
        syms.append((int(m.group(2), 16), m.group(1)))
    if not syms:
        # gcc -Wl,-Map: "  .text          0x00000000       0x42  foo.obj(alloc.o)"
        for m in re.finditer(r"^\s*\.text\s+0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+(.+)$", text, re.M):
            tail = m.group(2).split()
            name = tail[-1] if tail else ""
            if name and not name.startswith("0x"):
                syms.append((int(m.group(1), 16), name))
    syms.sort()
    return syms


def resolve(syms, addr):
    hit = None
    for s_addr, name in syms:
        if s_addr <= addr:
            hit = (s_addr, name)
        else:
            break
    return hit


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map", help="the .map file, a library path next to one, or a game id")
    ap.add_argument("addr", help="an address, decimal or 0x-prefixed hex")
    ap.add_argument("--rva", action="store_true", help="the address is an RVA, not an absolute one")
    ap.add_argument("--base", type=lambda s: int(s, 0), metavar="ADDR",
                    help=f"the image's actual load base, for a fault address from a moved DLL "
                         f"(default: the MSVC preferred base {DEFAULT_BASE:#x})")
    args = ap.parse_args()

    map_path = args.map
    if os.path.splitext(map_path)[1] != ".map":
        for cand in (map_path + ".map", os.path.splitext(map_path)[0] + ".map",
                     os.path.join("build", "emu", map_path, map_path + ".map")):
            if os.path.exists(cand):
                map_path = cand
                break
    if not os.path.exists(map_path):
        sys.exit(f"no map file: {args.map} - build first (python tools/emu/emu.py build <game>)")
    with open(map_path, errors="replace") as f:
        syms = parse_map(f.read())
    if not syms:
        sys.exit(f"no symbols parsed from {map_path}")
    map_base = max(DEFAULT_BASE, syms[0][0] & ~0xFFFFF) if os.name == "nt" else syms[0][0] & ~0xFFF

    addr = int(args.addr, 0)
    # Candidate interpretations, most likely first: the raw number (an MSVC report or
    # an unmoved DLL), an RVA onto the preferred base, or a rebased fault address.
    queries = [("as given", addr)]
    if args.rva:
        queries = [("rva", addr + map_base)]
    elif args.base is not None:
        queries = [("rebased", addr - args.base + map_base)]
    else:
        queries.append(("as rva", addr + map_base))

    for label, q in queries:
        hit = resolve(syms, q)
        if hit:
            print(f"{hit[1]}+{q - hit[0]:#x}  ({label}; symbol at {hit[0]:#x}, {map_path})")
            return
    sys.exit(f"no symbol at or before {args.addr} in {map_path}")


if __name__ == "__main__":
    main()
