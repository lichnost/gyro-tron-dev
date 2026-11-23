"""Gyro-Tron dev - an MCP server for end-to-end .tat game development.

Runs over stdio (newline-delimited JSON-RPC, the MCP framing) and wraps the repo's own
toolchain: the emulator (compile + deterministic screenshots), mktat.py (packaging) and
the manager/tatlink for installing to a watch. Standard library only; Pillow is needed
just for the placeholder icon that create_game generates.

The repo root is found by walking up from the working directory (marker: tools/mktat.py
plus components/tat_api), or set GYROTRON_ROOT to point at the clone directly.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import time
import traceback
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tatref

SERVER_NAME = "gyro-tron-dev"
SERVER_VERSION = "0.13.0"
PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")

ROOT = os.environ.get("GYROTRON_ROOT") or ""
EMU_SPECIAL = ("@boot", "@power", "@hold", "@home", "@update")


# ---------------------------------------------------------------- the repo root

def find_root():
    """The clone we work on. Env wins; otherwise walk up from the working directory.
    Last resort: this plugin repo's own sdk/ - the standalone install (no firmware
    clone anywhere above) keeps working no matter where the agent's cwd is."""
    global ROOT
    if ROOT and os.path.isfile(os.path.join(ROOT, "tools", "mktat.py")):
        return ROOT
    ROOT = ""
    d = os.getcwd()
    for _ in range(8):
        if os.path.isfile(os.path.join(d, "tools", "mktat.py")) and \
           os.path.isfile(os.path.join(d, "components", "tat_api", "include", "tat", "tat_api.h")):
            ROOT = d
            return ROOT
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    sdk = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sdk")
    if os.path.isfile(os.path.join(sdk, "tools", "mktat.py")):
        ROOT = sdk
    return ROOT


def p(*parts):
    return os.path.join(ROOT, *parts)


def api_constants():
    """TAT_API_MAJOR / TAT_API_MINOR straight from the header, so this never goes stale."""
    out = {"major": 1, "minor": 0}
    try:
        text = open(p("components", "tat_api", "include", "tat", "tat_api.h"),
                    encoding="utf-8", errors="replace").read()
        m = re.search(r"#define TAT_API_MAJOR\s+(\d+)", text)
        if m:
            out["major"] = int(m.group(1))
        m = re.search(r"#define TAT_API_MINOR\s+(\d+)", text)
        if m:
            out["minor"] = int(m.group(1))
    except OSError:
        pass
    return out


def export_list():
    """The C library a loaded game may call by name, read from the loader's export table.
    This is the one list that grows as games reach for more, so it is read rather than
    restated. Compiler helpers (__divsf3 and friends) stay out - no game names them."""
    try:
        text = _read(p("components", "loader", "exports.c"))
    except OSError:
        return None
    names = [n for n in re.findall(r"EXPORT\((\w+)\),", text) if not n.startswith("__")]
    return names or None


def api_surface():
    """The member names of tat_api_t, read live from the header - the API a game can
    actually call. GAME_API.md is a design spec and runs ahead of the header (it has
    documented sheet_free through two game builds the header never shipped), so the
    two are compared rather than trusted."""
    try:
        text = _read(p("components", "tat_api", "include", "tat", "tat_api.h"))
    except OSError:
        return None
    m = re.search(r"typedef struct tat_api \{(.*?)\} tat_api_t;", text, re.S)
    if not m:
        return None
    return re.findall(r"\(\s*\*\s*(\w+)\s*\)\s*\(", m.group(1))


def api_doc_drift():
    """(doc_only, header_only) - where docs/reference/GAME_API.md's ABI map and the header
    disagree. doc_only: functions the doc names that the header does not have
    (writing a call to one compiles nowhere; duckhunt wrote three sheet_free calls
    against the doc before finding out). header_only: header members the doc
    never mentions. The doc presents the api as the "what the OS provides" table
    (§4.2, `| Group | Members | Since |`), whose backticked names are the contract;
    if it ever goes back to carrying a full `} tat_api_t;` struct, that is parsed
    instead. None when a file is missing or neither shape can be found."""
    members = api_surface()
    if not members:
        return None
    try:
        doc = _read(p("docs", "reference", "GAME_API.md"))
    except OSError:
        return None
    end = doc.find("} tat_api_t;")
    if end >= 0:
        start = doc.rfind("typedef struct {", 0, end)
        if start < 0:
            return None
        block = doc[start:end]
    else:
        head = doc.find("| Group | Members | Since |")
        if head < 0:
            return None
        rows = []
        for line in doc[head:].splitlines()[1:]:
            if not line.startswith("|"):
                break
            rows.append(line)
        block = "\n".join(rows)
    doc_names = set(re.findall(r"`(\w+)", block))
    # The table's prose backticks words that are not api calls - gestures' results,
    # save_get described as "get", the ASST section, the unload event. A real member
    # gone from the header would not be on this list; one of these words becoming a
    # member is fine, it just stops being filtered.
    prose = {"ASST", "get", "set", "size", "tap", "swipe_left", "swipe_right", "unload"}
    doc_names -= prose
    return {"doc_only": sorted(n for n in doc_names if n not in members),
            "header_only": sorted(n for n in members if n not in doc_names)}


# ---------------------------------------------------------------- subprocesses

def run(cmd, timeout, cwd=None):
    """Run a tool and keep the tails. MSVC on a Russian locale emits non-UTF-8
    diagnostics that have killed output readers before - errors='replace' always."""
    try:
        r = subprocess.run(cmd, cwd=cwd or ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return {"returncode": r.returncode, "stdout": tail(r.stdout), "stderr": tail(r.stderr)}
    except subprocess.TimeoutExpired:
        return {"returncode": -1, "stdout": "", "stderr": f"timed out after {timeout}s"}
    except FileNotFoundError as e:
        return {"returncode": -1, "stdout": "", "stderr": f"not found: {e}"}


def tail(s, lines=60, chars=8000):
    if not s:
        return ""
    s = s.rstrip()
    ls = s.splitlines()
    if len(ls) > lines:
        s = "\n... (%d lines earlier dropped) ...\n" % (len(ls) - lines) + "\n".join(ls[-lines:])
    return s[:chars]


def py(*args, timeout=600):
    return run([sys.executable, *args], timeout)


def toolchain_report():
    """What this machine can do, so the agent knows before it tries to package."""
    rep = {"emulator": "ok", "mktat": "not checked"}
    if find_xtensa_gcc():
        rep["mktat"] = "ok"
    else:
        rep["mktat"] = ("xtensa-esp32s3-elf-gcc not found; .tat packaging needs the "
                        "Espressif toolchain (C:\\Espressif\\tools\\xtensa-esp-elf or ~/.espressif)")
    try:
        import PIL  # noqa: F401
        rep["pillow"] = "ok"
    except ImportError:
        rep["pillow"] = "missing; create_game cannot draw a placeholder icon"
    return rep


def find_xtensa_gcc():
    home = os.path.expanduser("~")
    for base in (r"C:\Espressif\tools\xtensa-esp-elf", os.path.join(home, ".espressif", "tools", "xtensa-esp-elf")):
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, _ in os.walk(base):
            if os.path.basename(dirpath) == "bin" and "xtensa-esp32s3" in dirpath:
                for name in os.listdir(dirpath):
                    if name.startswith("xtensa-esp32s3-elf-gcc"):
                        return os.path.join(dirpath, name)
            # Do not descend into the tools' own trees too far.
            if dirpath.count(os.sep) - base.count(os.sep) > 6:
                dirnames[:] = []
    return None


# ---------------------------------------------------------------- tool implementations

def game_json_path(game_id):
    return p("games", game_id, "game.json")


def read_game_json(game_id):
    try:
        with open(game_json_path(game_id), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def tool_create_game(a):
    gid = a["id"]
    name = a["name"]
    author = a["author"]
    accent = a.get("accent", [60, 220, 120])
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,14}", gid):
        return {"error": "id must be lowercase [a-z0-9_], start with a letter, max 15 chars "
                         "- it is the package id and the save namespace"}
    if len(name) > 23 or len(author) > 23:
        return {"error": "name and author max 23 chars each (24-byte package fields, NUL kept)"}
    if len(accent) != 3 or not all(isinstance(v, int) and 0 <= v <= 255 for v in accent):
        return {"error": "accent must be [r, g, b], 0..255 each"}
    if os.path.exists(p("games", gid)):
        return {"error": f"games/{gid} already exists"}
    if os.path.isdir(p("games")):
        for entry in os.listdir(p("games")):
            other = read_game_json(entry) if os.path.isfile(game_json_path(entry)) else None
            if other and other.get("id") == gid:
                return {"error": f"id '{gid}' is already used by games/{entry}"}

    api = api_constants()
    game_dir = p("games", gid)
    os.makedirs(game_dir)
    subs = {"__ID__": gid, "__NAME__": name.upper(), "__AUTHOR__": author,
            "__API_MAJOR__": str(api["major"]), "__API_MINOR__": str(api["minor"]),
            "__ACCENT_R__": str(accent[0]), "__ACCENT_G__": str(accent[1]),
            "__ACCENT_B__": str(accent[2])}
    src = tatref.GAME_TEMPLATE_C
    for k, v in subs.items():
        src = src.replace(k, v)
    with open(os.path.join(game_dir, f"{gid}.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    js = tatref.GAME_JSON_TEMPLATE
    for k, v in subs.items():
        js = js.replace(k, v)
    with open(os.path.join(game_dir, "game.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(js)

    icon_result = write_placeholder_icon(os.path.join(game_dir, "icon.png"), accent)
    out = {"created": [f"games/{gid}/game.json", f"games/{gid}/{gid}.c", f"games/{gid}/icon.png"],
           "icon": icon_result,
           "next": [f"Edit games/{gid}/{gid}.c - the template is a working tilt game.",
                    f"Ask me to compile_game('{gid}') and screenshot it."]}
    if icon_result.get("error"):
        out["next"].insert(0, "Put a 210x210 icon.png (max 4096 bytes) in the game folder "
                              "- the automatic placeholder failed.")
    return out


def write_placeholder_icon(path, accent):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return {"error": "Pillow is not installed; draw games/<id>/icon.png yourself "
                         "(210x210, circle, max 4096 bytes)"}
    s = 210
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # A disc in the game's accent with a dark rim and a dot marking up, echoing the
    # manager app's own icon. Drawn at 4x then boxed down so the edge is smooth.
    big = s * 4
    b = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    bd = ImageDraw.Draw(b)
    pad = int(big * 0.04)
    acc = tuple(accent) + (255,)
    dark = tuple(int(c * 0.45) for c in accent) + (255,)
    bd.ellipse((pad, pad, big - pad, big - pad), fill=dark)
    inset = int(big * 0.10)
    bd.ellipse((inset, inset, big - inset, big - inset), fill=acc)
    dot = int(big * 0.055)
    cy = int(big * 0.28)
    bd.ellipse((big // 2 - dot, cy - dot, big // 2 + dot, cy + dot), fill=(255, 255, 255, 230))
    img = b.resize((s, s), Image.LANCZOS)
    # The 4096-byte budget is the launcher's link-frame limit: downscale then quantise,
    # or the antialiased edge alone costs more than the whole icon may.
    img = img.convert("P", palette=Image.ADAPTIVE, colors=16)
    img.save(path, format="PNG", optimize=True)
    size = os.path.getsize(path)
    if size > 4096:
        return {"error": f"placeholder icon came out {size} bytes; shrink it to 4096"}
    return {"path": path.replace("\\", "/"), "bytes": size, "note": "placeholder - replace with real art"}


def tool_list_games(_a=None):
    if not ROOT:
        return {"error": no_root_msg()}
    games = []
    gdir = p("games")
    if os.path.isdir(gdir):
        for entry in sorted(os.listdir(gdir)):
            js = read_game_json(entry)
            if not js:
                continue
            src = os.path.join(gdir, entry, f"{entry}.c")
            if not os.path.isfile(src):
                c_files = [f for f in os.listdir(os.path.join(gdir, entry)) if f.endswith(".c")]
                src = os.path.join(gdir, entry, c_files[0]) if c_files else None
            games.append({
                "folder": entry, "id": js.get("id"), "name": js.get("name"),
                "version": js.get("version"), "accent": js.get("accent"),
                # Missing status means production - only freshly created descriptors carry "dev".
                "status": js.get("status", "production"),
                "api": f'{js.get("api_major")}.{js.get("api_minor")}',
                "source_lines": sum(1 for _ in open(src, encoding="utf-8", errors="replace")) if src else 0,
                "has_assets": os.path.isdir(os.path.join(gdir, entry, "assets")),
            })
    return {"games": games, "toolchain": toolchain_report()}


def no_root_msg():
    return ("Gyro-Tron repo root not found from the working directory. Set GYROTRON_ROOT "
            "in the MCP server config to the clone's path.")


def check_game_id(gid):
    if not ROOT:
        return no_root_msg()
    if not read_game_json(gid):
        return f"games/{gid}/game.json not found under {ROOT}"
    return None


def tool_compile_game(a):
    gid = a["game_id"]
    if gid not in EMU_SPECIAL and (err := check_game_id(gid)):
        return {"error": err}
    # The emulator compiles the game's real C against the firmware's own canvas, font and
    # API host for the PC - the fastest honest signal that a game builds, no xtensa needed.
    t0 = time.time()
    # One argv element for the script: python must see tools/emu/emu.py, not a "tools" dir.
    r = py(os.path.join("tools", "emu", "emu.py"), "build", gid, timeout=600)
    out = {"game": gid, "ok": r["returncode"] == 0, "seconds": round(time.time() - t0, 1),
           "stdout": r["stdout"], "stderr": r["stderr"]}
    if not out["ok"]:
        out["hint"] = ("Compile errors above. Common causes: C++isms (games are plain C), "
                       "file-scope const that is not a constant expression, missing include of "
                       "tat/tat_api.h, use of headers outside tat_api.h + libc.")
    return out


def tool_build_package(a):
    gid = a["game_id"]
    if err := check_game_id(gid):
        return {"error": err}
    # A GB ROM-cartridge descriptor (game.json with "template" and "rom") has no sources of
    # its own: kits/gbboy/tools/gbmake.py fetches the ROM and packs it on the gbboy template package.
    js = read_game_json(gid) or {}
    gb_descriptor = "template" in js or "rom" in js
    args = ([os.path.join("kits", "gbboy", "tools", "gbmake.py"), gid] if gb_descriptor
            else [os.path.join("tools", "mktat.py"), os.path.join("games", gid)])
    if a.get("skin") and not gb_descriptor:
        args += ["--skin", a["skin"]]
    t0 = time.time()
    # GB descriptors may download a ROM first, hence the longer timeout.
    r = py(*args, timeout=900 if gb_descriptor else 300)
    out_path = p("build", f"{js.get('id', gid)}.tat")
    out = {"game": gid, "ok": r["returncode"] == 0, "seconds": round(time.time() - t0, 1),
           "package": out_path.replace("\\", "/"),
           # Surfaced so an agent packing by name sees it picked a dev game - fine when
           # asked for by id, but dev games never belong in bulk build/install sweeps.
           "status": js.get("status", "production"),
           "stdout": r["stdout"], "stderr": r["stderr"]}
    if out["ok"] and os.path.isfile(out_path):
        out["bytes"] = os.path.getsize(out_path)
        out["validation"] = tool_validate_package({"path": out_path})
    elif not find_xtensa_gcc():
        out["hint"] = ("The xtensa toolchain was not found. mktat.py looks in "
                       "C:\\Espressif\\tools\\xtensa-esp-elf and ~/.espressif; --gcc overrides. "
                       "compile_game() still works - it builds for the PC.")
    return out


def golden_compare(shot_path, golden_path, tol=0):
    """(report) - one captured shot against its golden image. The emulator is
    deterministic (same script + seed = same pixels), so a mismatch is a real
    regression: the default tolerance is 0, and tol only exists for shots with a
    legitimately nondeterministic pixel or two. A mismatch writes a diff mask PNG
    (mismatching pixels in red) next to the shot, so what moved is visible."""
    import numpy as np
    from PIL import Image
    shot = Image.open(shot_path).convert("RGB")
    gold = Image.open(golden_path).convert("RGB")
    if shot.size != gold.size:
        return {"golden": golden_path.replace("\\", "/"), "ok": False,
                "error": f"size mismatch: shot {shot.size} vs golden {gold.size}"}
    diff = np.abs(np.asarray(shot, dtype=np.int16)
                  - np.asarray(gold, dtype=np.int16)).max(axis=2)
    bad = diff > int(tol)
    n = int(bad.sum())
    rep = {"golden": golden_path.replace("\\", "/"), "ok": n == 0, "tol": int(tol),
           "mismatch_px": n}
    if n:
        ys, xs = np.nonzero(bad)
        rep["first_mismatch"] = [int(xs[0]), int(ys[0])]
        rep["max_delta"] = int(diff.max())
        vis = np.asarray(shot).copy()
        vis[bad] = (255, 0, 64)
        mask_path = os.path.splitext(shot_path)[0] + ".diff.png"
        Image.fromarray(vis).save(mask_path)
        rep["diff_mask"] = mask_path.replace("\\", "/")
    return rep


def tool_screenshot(a):
    gid = a["game_id"]
    script = a["script"]
    if gid not in EMU_SPECIAL and (err := check_game_id(gid)):
        return {"error": err}
    out_dir = a.get("out_dir") or p("build", "emu", gid)
    os.makedirs(out_dir, exist_ok=True)
    args = [os.path.join("tools", "emu", "emu.py"), "shot", gid, script, "--out", out_dir]
    if a.get("seed") is not None:
        args += ["--seed", str(a["seed"])]
    if a.get("fresh"):
        args += ["--fresh"]
    defines = a.get("defines") or []
    if not isinstance(defines, list) or \
            any(not isinstance(d, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", d)
                for d in defines):
        return {"error": "defines must be an array of C identifier strings (plain names; "
                         "the emulator builds a separately named library for them), "
                         "e.g. ['DH_DEBUG']"}
    for d in defines:
        args += ["--define", d]
    env = dict(os.environ)
    env.update({k: str(v) for k, v in (a.get("env") or {}).items()})
    t0 = time.time()
    try:
        r = subprocess.run([sys.executable, *args], cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=600, env=env)
        res = {"returncode": r.returncode, "stdout": tail(r.stdout), "stderr": tail(r.stderr)}
    except subprocess.TimeoutExpired:
        res = {"returncode": -1, "stdout": "", "stderr": "timed out after 600s"}
    except FileNotFoundError as e:
        res = {"returncode": -1, "stdout": "", "stderr": f"not found: {e}"}
    marks = re.findall(r"(?:shot|sheet)\s+(\S+\.(?:png|PNG))", script)
    written = []
    for name in dict.fromkeys(marks):
        fp = os.path.join(out_dir, name)
        if os.path.isfile(fp):
            written.append({"path": fp.replace("\\", "/"), "bytes": os.path.getsize(fp)})
    out = {"game": gid, "ok": res["returncode"] == 0,
           "seconds": round(time.time() - t0, 1),
           "out_dir": out_dir.replace("\\", "/"),
           "images": written,
           "log": res["stdout"], "stderr": res["stderr"]}
    if not out["ok"]:
        out["hint"] = ("The build or the run failed - see log/stderr. Saves from earlier runs "
                       "persist in build/emu/<game>/save.txt; pass fresh=true to forget them.")
    elif not written:
        out["hint"] = "No 'shot <file.png>' steps in the script, so nothing was captured."

    # Golden-image regression: the assertion layer the screenshot suite never had.
    # The duckhunt build's states.py produced 18 screenshots for eyeballing with zero
    # checks; here each shot is compared against a golden under games/<id>/tests/golden/
    # (update_golden=true re-baselines after an intended change).
    gmode = a.get("golden")
    update = bool(a.get("update_golden"))
    if gmode or update:
        gdir = gmode if isinstance(gmode, str) and gmode.strip() \
            else p("games", gid, "tests", "golden")
        tol = a.get("golden_tol", 0)
        if not isinstance(tol, int) or isinstance(tol, bool) or tol < 0:
            return {"error": f"golden_tol must be a non-negative integer, not {tol!r}"}
        greps = []
        if update:
            os.makedirs(gdir, exist_ok=True)
            for w in written:
                dst = os.path.join(gdir, os.path.basename(w["path"]))
                shutil.copyfile(w["path"], dst)
                greps.append({"shot": os.path.basename(w["path"]), "ok": True,
                              "golden": dst.replace("\\", "/"), "updated": True})
        else:
            for w in written:
                dst = os.path.join(gdir, os.path.basename(w["path"]))
                if not os.path.isfile(dst):
                    greps.append({"shot": os.path.basename(w["path"]), "ok": False,
                                  "golden": dst.replace("\\", "/"),
                                  "note": "no golden yet - re-run with update_golden=true "
                                          "once the shot looks right"})
                    continue
                greps.append(golden_compare(w["path"], dst, tol=tol))
        out["golden"] = {"dir": gdir.replace("\\", "/"), "results": greps,
                         "ok": all(g["ok"] for g in greps) if greps else False}
        if written:
            out["ok"] = out["ok"] and out["golden"]["ok"]
    return out


# ---- .tat validation, per docs/reference/GAME_API.md section 2


def tool_validate_package(a):
    path = a["path"]
    try:
        data = open(path, "rb").read()
    except OSError as e:
        return {"error": f"cannot read {path}: {e}"}
    errors, checks = [], []
    if len(data) < 96 or data[:7] != b"TATPKG\x00":
        return {"error": "not a .tat package (bad magic)"}
    fmt_ver = data[7]
    checks.append(("format version", fmt_ver == 1, f"{fmt_ver}"))
    if fmt_ver != 1:
        errors.append(f"unknown format version {fmt_ver}")

    (total_size,) = _u32(data, 12)
    kind = data[16]
    api_maj = _u16(data, 18)[0]
    api_min = _u16(data, 20)[0]
    pkg_ver = _u16(data, 22)[0]
    pid = data[24:40].split(b"\x00")[0].decode("ascii", "replace")
    name = data[40:64].split(b"\x00")[0].decode("utf-8", "replace")
    author = data[64:88].split(b"\x00")[0].decode("utf-8", "replace")
    accent = tuple(data[88:91])
    (section_count,) = _u16(data, 92)

    hdr_crc = zlib.crc32(data[12:96 + 16 * section_count]) & 0xFFFFFFFF
    (stored_crc,) = _u32(data, 8)
    checks.append(("header crc32", hdr_crc == stored_crc,
                   f"stored {stored_crc:#010x} computed {hdr_crc:#010x}"))
    if hdr_crc != stored_crc:
        errors.append("header CRC mismatch")
    checks.append(("total_size", total_size == len(data), f"{total_size} vs {len(data)}"))

    sections = []
    for i in range(section_count):
        off = 96 + 16 * i
        (soff,) = _u32(data, off + 4)
        (ssize,) = _u32(data, off + 8)
        (scrc,) = _u32(data, off + 12)
        label = data[off:off + 4].decode("ascii", "replace")
        body = data[soff:soff + ssize]
        ok = len(body) == ssize and (zlib.crc32(body) & 0xFFFFFFFF) == scrc
        if not ok:
            errors.append(f"section {label}: bad size or CRC")
        info = {"type": label, "offset": soff, "size": ssize, "crc_ok": ok}
        if label == "ASST" and ssize >= 16:
            info["asset"] = body[:16].split(b"\x00")[0].decode("utf-8", "replace")
        sections.append(info)

    api = api_constants()
    compatible = None
    if kind == 1:
        compatible = (api_maj == api["major"] and api_min <= api["minor"])
        checks.append(("api version", compatible,
                       f"package {api_maj}.{api_min}, this repo {api['major']}.{api['minor']}"))
        if not compatible:
            errors.append("api version: this watch firmware would grey the icon out")
    else:
        # The loader refuses anything but KIND_GAME, so the validator says the same.
        errors.append(f"kind {kind}: only KIND_GAME (1) exists, values 2+ are unassigned")

    icon = {}
    for s in sections:
        if s["type"] == "ICON":
            png = data[s["offset"]:s["offset"] + s["size"]]
            w = h = None
            if png[:8] == b"\x89PNG\r\n\x1a\n" and len(png) > 24:
                w = int.from_bytes(png[16:20], "big")
                h = int.from_bytes(png[20:24], "big")
            icon = {"bytes": s["size"], "png": png[:8] == b"\x89PNG\r\n\x1a\n", "width": w, "height": h}
            if s["size"] > 4096:
                errors.append(f"icon is {s['size']} bytes, limit 4096")
            if w and (w > 232 or h > 232):
                errors.append(f"icon is {w}x{h}, launcher decodes at most 232x232")
            if w and (w < 200 or h < 200):
                errors.append(f"icon is {w}x{h}, expected 210x210")

    return {
        "path": path.replace("\\", "/"), "ok": not errors, "errors": errors,
        "kind": "GAME" if kind == 1 else kind, "id": pid, "name": name, "author": author,
        "version": pkg_ver, "accent_rgb": list(accent), "bytes": len(data),
        "api": {"major": api_maj, "minor": api_min},
        "icon": icon, "sections": sections,
        "checks": [{"check": c, "ok": ok, "detail": d} for c, ok, d in checks],
    }


def _u32(b, off):
    return (int.from_bytes(b[off:off + 4], "little"),)


def _u16(b, off):
    return (int.from_bytes(b[off:off + 2], "little"),)


def _watch_diagnostics():
    """Best-effort facts for install failures: where a manager was looked for, which
    serial ports exist (a watch on USB shows up as one - when pyserial is installed,
    which it need not be). Every entry answers one question the duckhunt build could
    only guess at with no watch attached."""
    diag = {}
    exe = shutil.which("gyrotron-manager")
    diag["manager_on_path"] = exe or None
    cand = p("app", "GyroTron.Manager", "publish", "gyrotron-manager.exe")
    diag["manager_published"] = cand if os.path.isfile(cand) else None
    diag["dotnet"] = shutil.which("dotnet") or None
    try:
        from serial.tools.list_ports import comports
        ports = [p.device for p in comports()]
        diag["serial_ports"] = ports or []
        diag["serial_note"] = ("a connected watch shows up as a COM port; an empty list "
                               "usually means the cable, not the package")
    except ImportError:
        diag["serial_ports"] = None
        diag["serial_note"] = "pyserial not installed in this Python - ports unknown"
    except Exception as e:   # a driver oddity must not sink the install attempt
        diag["serial_ports"] = None
        diag["serial_note"] = f"port listing failed: {e!r}"
    return diag


def tool_install_to_watch(a):
    path = a["path"]
    if not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    if not os.path.isfile(path):
        return {"error": f"no such file: {path}"}
    v = tool_validate_package({"path": path})
    if not v.get("ok", False):
        return {"error": "package failed validation; fix before installing", "validation": v}

    diag = _watch_diagnostics()
    exe = diag["manager_on_path"] or diag["manager_published"]
    if exe:
        r = run([exe, "install", path], timeout=300)
        out = {"ok": r["returncode"] == 0, "via": exe.replace("\\", "/"),
               "stdout": r["stdout"], "stderr": r["stderr"]}
    elif diag["dotnet"]:
        r = run([diag["dotnet"], "run", "--project",
                 p("app", "GyroTron.Manager"), "--", "install", path], timeout=600)
        out = {"ok": r["returncode"] == 0, "via": "dotnet run --project app/GyroTron.Manager",
               "stdout": r["stdout"], "stderr": r["stderr"]}
    else:
        return {"ok": False,
                "hint": "No gyrotron-manager found. Either publish it "
                        "(cd app/GyroTron.Manager && dotnet publish -c Release -r win-x64 -o publish) "
                        "or install by hand from the manager app GUI with the watch plugged in.",
                "package": path.replace("\\", "/")}
    if not out["ok"]:
        ports = diag.get("serial_ports")
        out["hint"] = ("Install failed. Is the watch connected over USB and not mid-something? "
                       "gyrotron-manager list tells whether a watch answers at all.")
        out["diagnostics"] = diag
        if ports is not None and not ports:
            out["hint"] += (" No serial ports at all - check the cable and whether the "
                            "watch is awake, then retry or run .\\update.ps1 to build, "
                            "install and verify in one go.")
    return out


def hw_debug(*args, timeout=120):
    r = py(p("tools", "hw_debug.py"), *args, timeout=timeout)
    return {"ok": r["returncode"] == 0, "stdout": r["stdout"], "stderr": r["stderr"]}


def tool_get_log(a):
    return hw_debug(*(["--port", a["port"]] if a.get("port") else []), "--log")


def tool_watch_control(a):
    """Everything between install and read-log goes through hw_debug.py - one driver for
    the real watch, not a second one here. It runs rescan, launch, shot, script, reboot."""
    args = []
    if a.get("rescan"):
        args.append("--rescan")
    if a.get("game"):
        args += ["--game", a["game"]]
    if a.get("shot"):
        args += ["--shot", a["shot"]]
    if a.get("script"):
        args += ["--script", a["script"]]
    if a.get("reboot"):
        args.append("--reboot")
    if a.get("serial") is not None:
        # The panic catcher: one input, then raw UART (log lines, backtraces).
        args += ["--serial", a["serial"], "--seconds", str(a.get("seconds", 20))]
    if not args:
        return {"error": "nothing to do: give rescan, game, script, shot, reboot or serial"}
    return hw_debug(*(["--port", a["port"]] if a.get("port") else []), *args, timeout=300)


TOOLS = [
    {
        "name": "get_log",
        "description": ("Read the tail of a connected watch's log over USB (hw_debug.py --log). "
                        "The fastest way to see what the real hardware is saying."),
        "inputSchema": {"type": "object",
                        "properties": {"port": {"type": "string", "description": "default: auto-detect"}}},
    },
    {
        "name": "watch_control",
        "description": ("Drive a connected watch over USB: rescan the games folder, launch a game "
                        "by id, take a screenshot, run a script (tap/hold/swipe/btn/tilt/app <id>/"
                        "shot/waitpx/waitlog), reboot, or capture raw serial after one input "
                        "(log lines, panics) - in that order. rescan and reboot need firmware "
                        "that has those control words."),
        "inputSchema": {"type": "object",
                        "properties": {
                            "port": {"type": "string", "description": "default: auto-detect"},
                            "rescan": {"type": "boolean"},
                            "game": {"type": "string", "description": "game id, resolved at launch"},
                            "shot": {"type": "string", "description": "PNG path to write"},
                            "script": {"type": "string"},
                            "reboot": {"type": "boolean"},
                            "serial": {"type": "string", "description": "input to send (e.g. \"app=3\") or \"\" for none, then print raw serial"},
                            "seconds": {"type": "number", "description": "how long --serial listens (default 20)"},
                        }},
    },
    {
        "name": "create_game",
        "description": ("Create a new Gyro-Tron game skeleton at games/<id>/ in the repo: "
                        "game.json, <id>.c (a working tilt-game template against tat_api.h) and a "
                        "210x210 placeholder icon.png. The template compiles and runs as-is."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {"type": "string", "description": "package id: lowercase [a-z0-9_], starts with a letter, max 15 chars; also the save namespace"},
                "name": {"type": "string", "description": "launcher name, max 23 chars, conventionally ALL CAPS"},
                "author": {"type": "string", "description": "author shown in the manager app, max 23 chars"},
                "accent": {"type": "array", "items": {"type": "integer", "minimum": 0, "maximum": 255},
                           "description": "[r,g,b] launcher accent colour", "default": [60, 220, 120]},
            },
            "required": ["id", "name", "author"],
        },
    },
    {
        "name": "list_games",
        "description": ("List the games in the repo's games/ folder with their ids, versions, "
                        "maturity status (\"dev\" games are excluded from bulk build/install "
                        "sweeps - only an explicit pack or install by id touches them) and "
                        "source sizes, plus what the local toolchain can do."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "compile_game",
        "description": ("Compile a game by building it into the PC emulator (emu.py build): the "
                        "game's real C against the firmware's own canvas/font/API host. Fastest "
                        "honest build check; does not need the xtensa toolchain."),
        "inputSchema": {"type": "object",
                        "properties": {"game_id": {"type": "string"}},
                        "required": ["game_id"]},
    },
    {
        "name": "build_package",
        "description": ("Pack a game into build/<id>.tat with tools/mktat.py (needs the xtensa "
                        "toolchain) and validate the result. The link is checked against the "
                        "loader's frozen export list - a call outside it fails the pack here "
                        "instead of failing to load on the watch. Optional --skin folder swaps "
                        "the game's art at pack time."),
        "inputSchema": {"type": "object",
                        "properties": {"game_id": {"type": "string"},
                                       "skin": {"type": "string", "description": "optional skin folder passed to mktat --skin"}},
                        "required": ["game_id"]},
    },
    {
        "name": "screenshot",
        "description": ("Run a game (or a console screen: @boot @power @hold @home @update) in the "
                        "headless deterministic emulator and capture screenshots from a script. "
                        "Same script + seed = same pixels. Look at the returned PNG paths yourself "
                        "(you have vision). Script steps, ;-separated: wait <s>, tap <x> <y>, "
                        "touch/release <x> <y>, swipe <x0> <y0> <x1> <y1> [s], turn <deg>, "
                        "pitch <deg> (+ is looking down), yaw <deg>, tilt <ax> <ay> <az>, "
                        "button b [s], hold b, letgo, shot <file.png>, sheet <file.png>, "
                        "wait_pixel <x> <y> <#hex> [tol] [timeout_s] (run frames until the pixel "
                        "matches - robust scripts instead of hardcoded waits), "
                        "wait_log \"<regex>\" [timeout_s] (until the game's own log line shows). "
                        "defines builds the game with preprocessor symbols - a game's own "
                        "telemetry/autopilot switches (e.g. DH_DEBUG), surfaced in the log. "
                        "golden=true compares every shot against its golden PNG under "
                        "games/<id>/tests/golden/ (update_golden=true re-baselines), so a "
                        "regression is an assertion instead of an eyeball. Screen is 466x466."),
        "inputSchema": {"type": "object",
                        "properties": {
                            "game_id": {"type": "string"},
                            "script": {"type": "string", "description": "e.g. \"wait 0.5; tap 233 233; wait 2; shot menu.png\""},
                            "out_dir": {"type": "string", "description": "default build/emu/<game>"},
                            "seed": {"type": "integer"},
                            "fresh": {"type": "boolean", "description": "forget saved scores first"},
                            "env": {"type": "object", "description": "extra env vars, e.g. EMU_BATTERY, EMU_CHARGING, EMU_USB, EMU_OTA"},
                            "defines": {"type": "array", "items": {"type": "string"},
                                        "description": "preprocessor symbols to build the game with (repeatable --define), e.g. ['DH_DEBUG', 'DH_AUTOPLAY'] - the game's own telemetry/autopilot switches, visible in the returned log"},
                            "golden": {"description": "true = compare each shot against games/<id>/tests/golden/<name>.png; or a string = a custom golden directory", "anyOf": [{"type": "boolean"}, {"type": "string"}]},
                            "update_golden": {"type": "boolean", "description": "write the captured shots as the goldens instead of comparing (re-baseline after an intended change)"},
                            "golden_tol": {"type": "integer", "default": 0, "description": "per-channel pixel tolerance for the golden comparison; 0 (the default) means exact - the emulator is deterministic"},
                        },
                        "required": ["game_id", "script"]},
    },
    {
        "name": "validate_package",
        "description": ("Validate a .tat package: magic, header CRC, section CRCs, api version "
                        "against this repo's tat_api.h, icon size/dimensions. Run before installing."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string"}},
                        "required": ["path"]},
    },
    {
        "name": "install_to_watch",
        "description": ("Install a .tat onto a connected watch via gyrotron-manager (or dotnet "
                        "run). Validates first. Without a manager or a watch it explains the "
                        "manual route instead of failing silently."),
        "inputSchema": {"type": "object",
                        "properties": {"path": {"type": "string"}},
                        "required": ["path"]},
    },
]

RESOURCES = [
    {"uri": "tat://api/header", "name": "tat_api.h", "mimeType": "text/plain",
     "description": "The real game API header - the contract, the truth. Read this before writing code."},
    {"uri": "tat://api/docs", "name": "GAME_API.md", "mimeType": "text/markdown",
     "description": "Full design spec: packages, the ABI, the USB link, porting notes."},
    {"uri": "tat://constraints", "name": "Constraints digest", "mimeType": "text/markdown",
     "description": "The hard rules: round screen, palette canvas, plain C, memory rules, icon limits, shared UX."},
    {"uri": "tat://examples", "name": "Examples index", "mimeType": "text/markdown",
     "description": "The games in this repo, with sizes - pick one to read."},
]

RESOURCE_TEMPLATES = [
    {"uriTemplate": "tat://examples/{id}", "name": "Game source", "mimeType": "text/plain",
     "description": "A game's full .c source followed by its game.json."}]


def read_resource(uri):
    if uri == "tat://constraints":
        text = tatref.CONSTRAINTS_MD
        exp = export_list()
        # A missing exports.c must not leave the placeholder in front of an agent.
        text = text.replace("__EXPORT_LIST__",
                            ", ".join(exp) if exp else
                            "the float maths, the string/memory functions, snprintf, abs/labs")
        return text, "text/markdown"
    if not ROOT:
        raise RuntimeError(no_root_msg())
    if uri == "tat://api/header":
        return _read(p("components", "tat_api", "include", "tat", "tat_api.h")), "text/plain"
    if uri == "tat://api/docs":
        text = _read(p("docs", "reference", "GAME_API.md"))
        drift = api_doc_drift()
        if drift and drift["doc_only"]:
            # The doc is a design spec and runs ahead of the header; duckhunt wrote
            # sheet_free calls against it that the watch would have refused. Say so
            # where the agent is looking, not only in the selftest.
            text = ("> NOTE: this is the DESIGN SPEC - it also documents members the "
                    f"current header does not ship: {', '.join(drift['doc_only'])}. "
                    "tat://api/header is the truth a game must compile against.\n\n") + text
        return text, "text/markdown"
    if uri == "tat://examples":
        games = tool_list_games().get("games", [])
        lines = ["Games in this repo (ask for tat://examples/<folder> to read one):", ""]
        for g in games:
            lines.append(f"- {g['folder']:<14} {g['name'] or '':<16} {g['source_lines']:>5} lines"
                         f"  api {g['api']}{'  assets' if g['has_assets'] else ''}")
        lines += ["", "games/echo is the reference game - start there."]
        return "\n".join(lines), "text/markdown"
    m = re.fullmatch(r"tat://examples/([a-z0-9_@]+)", uri)
    if m:
        gid = m.group(1)
        gdir = p("games", gid)
        if not os.path.isdir(gdir):
            raise RuntimeError(f"no game games/{gid}")
        c_files = sorted(f for f in os.listdir(gdir) if f.endswith(".c"))
        parts = [f"===== games/{gid}/{f} =====\n\n" + _read(os.path.join(gdir, f)) for f in c_files]
        js = read_game_json(gid)
        if js:
            parts.append(f"===== games/{gid}/game.json =====\n\n" + json.dumps(js, indent=2))
        return "\n\n".join(parts), "text/plain"
    raise RuntimeError(f"unknown resource: {uri}")


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def dispatch_tool(name, args):
    if not ROOT and name != "list_games":
        return {"error": no_root_msg()}
    impl = {
        "create_game": tool_create_game,
        "list_games": tool_list_games,
        "compile_game": tool_compile_game,
        "build_package": tool_build_package,
        "screenshot": tool_screenshot,
        "validate_package": tool_validate_package,
        "install_to_watch": tool_install_to_watch,
        "get_log": tool_get_log,
        "watch_control": tool_watch_control,
    }[name]
    return impl(args or {})


def reply(msg_id, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": msg_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    send(msg)


def send(obj):
    sys.stdout.buffer.write(json.dumps(obj, ensure_ascii=False).encode("utf-8"))
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def log(*a):
    print(SERVER_NAME + ":", *a, file=sys.stderr, flush=True)


def handle(msg):
    method = msg.get("method")
    msg_id = msg.get("id")
    params = msg.get("params") or {}
    is_notification = msg_id is None

    if method == "initialize":
        requested = params.get("protocolVersion")
        version = requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
        root = find_root()
        result = {
            "protocolVersion": version,
            "capabilities": {
                "tools": {},
                "resources": {"subscribe": False, "listChanged": False},
            },
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        }
        if root:
            result["instructions"] = (
                f"Gyro-Tron dev server, repo at {root}. Workflow: create_game -> edit the "
                f".c -> compile_game -> screenshot and look at the PNGs -> build_package -> "
                f"validate_package -> install_to_watch. Read tat://api/header and "
                f"tat://constraints before writing code.")
        else:
            result["instructions"] = ("Gyro-Tron dev server could not find the repo. Set "
                                      "GYROTRON_ROOT in this MCP server's config to the clone's path.")
        log("initialized; repo:", root or "NOT FOUND")
        return reply(msg_id, result)

    if is_notification:
        return  # notifications/initialized and friends need no answer

    try:
        if method == "ping":
            return reply(msg_id, {})
        if method == "tools/list":
            return reply(msg_id, {"tools": TOOLS})
        if method == "tools/call":
            name = params.get("name")
            try:
                out = dispatch_tool(name, params.get("arguments") or {})
                text = json.dumps(out, indent=2, ensure_ascii=False)
                return reply(msg_id, {"content": [{"type": "text", "text": text}],
                                      "isError": bool(out.get("error"))})
            except KeyError:
                return reply(msg_id, error={"code": -32602, "message": f"unknown tool: {name}"})
        if method == "resources/list":
            return reply(msg_id, {"resources": RESOURCES})
        if method == "resources/templates/list":
            return reply(msg_id, {"resourceTemplates": RESOURCE_TEMPLATES})
        if method == "resources/read":
            uri = params.get("uri", "")
            try:
                text, mime = read_resource(uri)
                return reply(msg_id, {"contents": [{"uri": uri, "mimeType": mime, "text": text}]})
            except RuntimeError as e:
                return reply(msg_id, error={"code": -32602, "message": str(e)})
        return reply(msg_id, error={"code": -32601, "message": f"method not supported: {method}"})
    except Exception:
        log(traceback.format_exc())
        return reply(msg_id, error={"code": -32603, "message": "internal error (see server log)"})


def main():
    find_root()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError as e:
            send({"jsonrpc": "2.0", "id": None,
                  "error": {"code": -32700, "message": f"parse error: {e}"}})
            continue
        handle(msg)


if __name__ == "__main__":
    main()
