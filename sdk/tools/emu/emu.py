#!/usr/bin/env python3
"""Run a Gyro-Tron game on a PC.

    python tools/emu/emu.py play echo                 a window: play it
    python tools/emu/emu.py shot echo "wait 1; tap 233 233; wait 2; shot a.png"
                                                      no window: drive it from a script

The game is built, with the console's own canvas, font, menus and game API around it, into
a library (see emu_core.cpp), and this drives that library a frame at a time. `play` opens
a round window the size of the watch's screen; `shot` runs a script at exactly sixty frames
a second of its own time, so the same script gives the same pictures every time, which is
what makes it usable for testing.

In the window:
    mouse                  your finger
    left / right           turn the watch like a wheel (hold shift for a finer touch)
    up / down              tip the top away from you / back toward you
    A / D                  look around the vertical axis: what a scope game sees as aiming
    space                  level again
    F                      lay the watch flat / hold it upright (it starts upright)
    B                      the PWR button          Esc or H   the BOOT button (quit)
    P                      save a picture to build/emu/

A script is steps separated by ; or newlines:
    wait <seconds>         run that long
    tap <x> <y>            a finger down and up (screen pixels, 466 x 466)
    touch <x> <y> / release      a finger down and held / let go
    swipe <x0> <y0> <x1> <y1> [seconds]
    tilt <ax> <ay> <az>    gravity as the game sees it, in g (+x right, +y down, +z out of the glass)
    turn <degrees>         the watch upright, turned like a wheel (right is positive)
    pitch <degrees>        ... and tipped, top toward you positive - the glass tips to
                           face the ground (aiming down), so a game's sight-line pitch
                           reads negative
    yaw <degrees>          ... and turned about the vertical, looking around; gravity does
                           not move, but the gyro does (a jump is one frame of fast turning)
    button b [seconds]     PWR, clicked or held
    shot <file.png>        save the screen
    sheet <file.png>       save every `shot` so far tiled into one picture
    wait_pixel <x> <y> <#hex> [tol] [seconds]
                           run frames until that pixel is within tol of the color -
                           scripts keyed to what is on screen instead of to one seed's
                           timings, which hardcoded waits cannot be
    wait_log "<regex>" [seconds]
                           run frames until the game has logged a matching line (its own
                           T->log output; also works with --quiet)
"""
import argparse
import ctypes
import glob
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "build", "emu")
SCREEN = 466
BTN_B = 2

# The console's sources that a game stands on, compiled unchanged.
CONSOLE_SOURCES = [
    "components/tat_api/tat_host.cpp",
    "components/engine/canvas.cpp",
    "components/engine/font5x7.cpp",
    "components/engine/polar.cpp",
    "components/engine/gfx.cpp",
    "components/engine/store.cpp",
    "components/engine/calib_fit.cpp",
    "components/engine/autocal.cpp",
    "components/wc_console/banner.cpp",
    "components/wc_console/pause_menu.cpp",
    "components/lodepng/lodepng.cpp",
]
# The console's own screens, which are not games: `emu.py play @boot`. Each screen
# compiles alongside just the console sources it stands on; @home adds the launcher
# and the code-drawn icons it hangs the carousel on; @update is the firmware-update
# screen with its pull-OTA client faked (EMU_OTA, EMU_USB - see emu_core.cpp); @store
# is the game store with its catalog fetcher faked (EMU_STORE - a local folder with
# index-watch.json and the packages, see emu_core.cpp); @cal is Settings > CALIBRATE
# straight up, for scripted runs with EMU_ACC_* faults injected.
SCREENS = {"@boot": "EMU_SCREEN_BOOT", "@power": "EMU_SCREEN_POWER", "@hold": "EMU_SCREEN_HOLD",
           "@home": "EMU_SCREEN_HOME", "@update": "EMU_SCREEN_UPDATE", "@store": "EMU_SCREEN_STORE",
           "@cal": "EMU_SCREEN_CAL"}
SCREEN_SOURCES = {
    "default": [
        "components/wc_console/boot_anim.cpp",
        "components/wc_console/power_menu.cpp",
    ],
    "@home": [
        "components/wc_console/launcher.cpp",
        "components/wc_console/icons.cpp",
    ],
    "@update": [
        "components/wc_console/update_app.cpp",
        "components/wc_console/wifi_setup.cpp",
    ],
    "@store": [
        "components/wc_console/store_app.cpp",
        "components/wc_console/store_index.cpp",
        "components/wc_console/wifi_setup.cpp",
        "components/wc_console/scroll_list.cpp",
    ],
    "@cal": [
        "components/wc_console/calibrate_app.cpp",
        "components/wc_console/tiltcal_store.cpp",
    ],
}
INCLUDES = [
    "tools/emu/stubs",
    "components/engine/include",
    "components/tat_api/include",
    "components/wc_console/include",
    "components/audio/include",
    "components/lodepng",
    "components/board/include",
    "components/storage/include",
    "components/net/include",
    "components/loader/include",
]


# ---------------------------------------------------------------------------- building

def find_msvc():
    """cl.exe and the environment it needs, without a developer prompt."""
    hits = sorted(glob.glob(r"C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe"))
    if not hits:
        return None
    cl = hits[-1]
    msvc = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(cl))))
    kits = r"C:\Program Files (x86)\Windows Kits\10"
    inc = sorted(glob.glob(os.path.join(kits, "Include", "*")))[-1]
    lib = sorted(glob.glob(os.path.join(kits, "Lib", "*")))[-1]
    env = dict(os.environ)
    env["INCLUDE"] = ";".join([os.path.join(msvc, "include")] + [os.path.join(inc, d) for d in ("ucrt", "shared", "um")])
    env["LIB"] = ";".join([os.path.join(msvc, "lib", "x64"), os.path.join(lib, "ucrt", "x64"), os.path.join(lib, "um", "x64")])
    return cl, env


def library_path(game, defines=()):
    ext = ".dll" if os.name == "nt" else ".dylib" if sys.platform == "darwin" else ".so"
    # A build with test switches is a different library: it must not be mistaken for the game.
    return os.path.join(OUT, game, game + "".join("+" + d for d in defines) + ext)


def build(game, force=False, verbose=False, extra_defines=()):
    defines = list(extra_defines)
    game_dir = None
    base = None
    if game in SCREENS:
        defines += [SCREENS[game]]
        game_sources = [os.path.join(ROOT, s) for s in SCREEN_SOURCES.get(game, SCREEN_SOURCES["default"])]
    else:
        game_dir = os.path.join(ROOT, "games", game)
        if not os.path.isdir(game_dir):
            sys.exit(f"no such game: games/{game} (or one of {', '.join(SCREENS)})")
        # Recursive, matching tools/mktat.py: a game may keep sources in subfolders
        # (e.g. a vendored emulator core under games/<id>/).
        game_sources = sorted(glob.glob(os.path.join(game_dir, "**", "*.c"), recursive=True))
        if not game_sources:
            # A GB ROM-cartridge descriptor (kits/gbboy/tools/gbmake.py) carries no sources: its code
            # is the template game's, and its ROM arrives as an asset (see Console below).
            with open(os.path.join(game_dir, "game.json")) as f:
                base = json.load(f).get("template")
            if not base:
                sys.exit(f"no .c files in games/{game} and no \"template\" in game.json")
            game_sources = sorted(glob.glob(os.path.join(ROOT, "games", base, "**", "*.c"), recursive=True))
            if not game_sources:
                sys.exit(f"template game games/{base} has no .c files")
    sources = [os.path.join(ROOT, s) for s in CONSOLE_SOURCES] + [os.path.join(HERE, "emu_core.cpp"),
                                                                 os.path.join(HERE, "emu_assets.c")] + game_sources
    lib = library_path(game, extra_defines)
    # A game's own headers (vendored shim, engine) change its binary just as much as its
    # sources do - editing one used to leave a stale DLL that only a touch of a .c exposed.
    headers = glob.glob(os.path.join(ROOT, "components", "*", "include", "**", "*.h"), recursive=True)
    for d in filter(None, [game_dir, os.path.join(ROOT, "games", base) if base else None]):
        headers += glob.glob(os.path.join(d, "**", "*.h"), recursive=True)
    newest = max(os.path.getmtime(p) for p in sources + headers + [os.path.abspath(__file__)])
    if not force and os.path.exists(lib) and os.path.getmtime(lib) >= newest:
        return lib

    work = os.path.join(OUT, game)
    os.makedirs(work, exist_ok=True)
    incs = [os.path.join(ROOT, i) for i in INCLUDES]
    # A game may ship its own include tree (vendored engine, shim headers) - angle-bracket
    # includes such as <SDL.h> need it on the path; quoted ones resolve on their own.
    game_inc = os.path.join(game_dir, "include") if game_dir else None
    if game_inc and os.path.isdir(game_inc):
        incs.append(game_inc)
    msvc = find_msvc() if os.name == "nt" else None
    # A game's forced prelude (include/cd_preload.h) remaps libc names the watch does
    # not export; the emulator applies it too, so both builds share one source.
    preload = os.path.join(game_inc, "cd_preload.h") if game_inc else ""
    forced = ["/FI" + preload] if game_inc and os.path.isfile(preload) else []
    t0 = time.time()
    if msvc:
        cl, env = msvc
        common = [cl, "/nologo", "/c", "/O2", "/MD", "/W3", "/wd4100", "/wd4996", "/wd4244", "/wd4305", "/wd4267", "/wd4018",
                  "/wd4146", "/D_USE_MATH_DEFINES", "/D_CRT_SECURE_NO_WARNINGS", "/utf-8", "/Fo" + work + os.sep]
        common += ["/I" + i for i in incs] + ["/D" + d for d in defines]
        # C and C++ in two goes: MSVC will not be told a standard for both at once.
        # The game's own C gets the forced prelude as well - the console's does not:
        # the console uses the real heap and stdio, only the game's libc is remapped.
        game_c = [s for s in sources if s.endswith(".c") and s in game_sources]
        console_c = [s for s in sources if s.endswith(".c") and s not in game_c]
        cpp_files = [s for s in sources if not s.endswith(".c")]
        # errors='replace': on a non-English Windows cl.exe emits locale-encoded
        # diagnostics that would kill the output reader mid-build.
        r = subprocess.run(common + ["/std:c17"] + console_c, env=env, capture_output=True, text=True,
                           errors="replace")
        if r.returncode == 0 and game_c:
            r = subprocess.run(common + forced + ["/std:c17"] + game_c, env=env, capture_output=True, text=True,
                               errors="replace")
        if r.returncode == 0:
            r = subprocess.run(common + ["/std:c++20", "/EHsc"] + cpp_files, env=env, capture_output=True, text=True,
                               errors="replace")
        if r.returncode == 0:
            objs = [os.path.join(work, os.path.splitext(os.path.basename(s))[0] + ".obj") for s in sources]
            # The map is what turns a crash address into a function - tools/emu/sym.py
            # resolves it; regenerate it with every link so it never goes stale.
            r = subprocess.run([cl, "/nologo", "/LD", "/MD", "/Fe" + lib] + objs +
                               ["/link", "/MAP", "/MAPINFO:EXPORTS"],
                               env=env, capture_output=True, text=True, cwd=work, errors="replace")
    else:
        cxx = shutil.which("g++") or shutil.which("clang++")
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cxx or not cc:
            sys.exit("no C++ compiler found: install gcc or clang (or the MSVC build tools on Windows)")
        objs = []
        for s in sources:
            o = os.path.join(work, os.path.basename(s) + ".o")
            is_c = s.endswith(".c")
            cmd = [cc if is_c else cxx, "-c", "-O2", "-fPIC", "-std=gnu17" if is_c else "-std=gnu++20", "-w", "-o", o, s]
            cmd += ["-I" + i for i in incs] + ["-D" + d for d in defines]
            if s in game_sources:
                cmd += forced
            r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
            if r.returncode:
                sys.exit(r.stdout + r.stderr)
            objs.append(o)
        r = subprocess.run([cxx, "-shared", "-o", lib, "-Wl,-Map," + lib + ".map"] + objs + ["-lm"],
                           capture_output=True, text=True, errors="replace")
    if r.returncode:
        # The compiler names every file as it goes; only what went wrong is worth reading.
        lines = [l for l in (r.stdout + r.stderr).splitlines() if "error" in l.lower() or "unresolved" in l.lower()]
        sys.exit("\n".join(lines[:40]) or (r.stdout + r.stderr))
    if verbose:
        print(f"built {os.path.relpath(lib, ROOT)} in {time.time() - t0:.1f} s")
    return lib


def load_tat(path):
    """Load a package the way the watch does: the real components/loader/loader.c, compiled
    for the PC against loadstubs/ (no MMU, no cache, no PSRAM - the probe never executes the
    code it loads). play and shot link a game straight in and never open a .tat, which is
    how a package the watch refused got shipped: the reason only existed inside loader.c.
    Needs gcc or clang - loader.c is GNU C, MSVC will not take its packed structs."""
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        sys.exit("no gcc or clang found: the loader probe needs one to compile loader.c")
    sources = [os.path.join(ROOT, "components", "loader", "loader.c"),
               os.path.join(ROOT, "components", "loader", "exports.c"),
               os.path.join(ROOT, "components", "loader", "sig.c"),
               os.path.join(ROOT, "components", "loader", "keys.c"),
               os.path.join(HERE, "loader_probe.c")]
    watched = sources + glob.glob(os.path.join(ROOT, "components", "**", "*.h"), recursive=True) \
        + glob.glob(os.path.join(HERE, "loadstubs", "**", "*.h"), recursive=True)
    exe = os.path.join(OUT, "loader-probe", "loader_probe" + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(p) for p in watched):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        incs = ["-I" + os.path.join(HERE, "loadstubs"),
                "-I" + os.path.join(ROOT, "components", "loader", "include"),
                "-I" + os.path.join(ROOT, "components", "tat_api", "include"),
                "-I" + os.path.join(ROOT, "components", "loader")]
        r = subprocess.run([cc, "-O2", "-o", exe] + incs + sources, capture_output=True,
                           text=True, errors="replace")
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
    r = subprocess.run([exe, path])
    return r.returncode


# ---------------------------------------------------------------------------- the console

class Console:
    """One game, running. step() is a frame; frame() is what is on the screen."""

    _runs = 0

    def __init__(self, game, seed=0, quiet=False, fresh=False, verbose=False, defines=(), assets=()):
        self.game = game
        lib = build(game, verbose=verbose, extra_defines=tuple(defines))
        # Loaded from a copy, so that the next build can replace the library while a window
        # still has the last one open.
        Console._runs += 1   # a second console in one process cannot overwrite a library still loaded
        self._copy = os.path.join(OUT, game, f"run-{os.getpid()}-{Console._runs}{os.path.splitext(lib)[1]}")
        shutil.copyfile(lib, self._copy)
        self.lib = ctypes.CDLL(self._copy)
        L = self.lib
        L.emu_add_asset.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        L.emu_options.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_int]
        L.emu_input.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint] + [ctypes.c_float] * 10
        L.emu_step.argtypes = [ctypes.c_float]
        L.emu_state_save.argtypes = [ctypes.c_char_p, ctypes.c_uint]
        L.emu_state_save.restype = ctypes.c_int
        L.emu_state_load.argtypes = [ctypes.c_char_p, ctypes.c_uint]
        L.emu_state_load.restype = ctypes.c_bool
        L.emu_log_take.argtypes = [ctypes.c_char_p, ctypes.c_int]
        L.emu_log_take.restype = ctypes.c_int
        L.emu_frame.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.emu_game_name.restype = ctypes.c_char_p
        # The game's own PCM render piles up in the library until taken (see emu_core.cpp).
        L.emu_audio_take.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
        L.emu_audio_take.restype = ctypes.c_int
        L.emu_audio_rate.restype = ctypes.c_int

        save = os.path.join(OUT, game, "save.txt")
        if fresh and os.path.exists(save):
            os.remove(save)
        L.emu_options(save.encode(), seed, 1 if quiet else 0)
        game_json = os.path.join(ROOT, "games", game, "game.json")
        meta = json.load(open(game_json)) if os.path.exists(game_json) else {}
        self._assets = []   # kept alive: the library holds pointers into them
        overrides = dict(assets)   # name -> path: stand-in files for the packaged ones
        for path in sorted(glob.glob(os.path.join(ROOT, "games", game, "assets", "*"))):
            name = os.path.basename(path)
            if name in overrides:
                path = overrides.pop(name)
            data = open(path, "rb").read()
            self._assets.append(data)
            L.emu_add_asset(name.encode(), data, len(data))
        for name, path in overrides.items():   # files the game dir does not carry at all
            data = open(path, "rb").read()
            self._assets.append(data)
            L.emu_add_asset(name.encode(), data, len(data))
        # A ROM-cartridge descriptor keeps its ROM out of the repo: kits/gbboy/tools/gbmake.py caches
        # it as build/roms/<id>.rom, and it is handed over under the name the template's code
        # asks for ("rom.gb"). --asset rom.gb=... still wins, for probing other dumps.
        if meta.get("template") and meta.get("rom") and not any(n == "rom.gb" for n, _ in assets):
            cache = os.path.join(ROOT, "build", "roms", json.load(open(game_json))["id"] + ".rom")
            if not os.path.exists(cache):
                sys.exit(f"no ROM cached for {game}: run python kits/gbboy/tools/gbmake.py {game} first "
                         f"(or pass --asset rom.gb=<path>)")
            data = open(cache, "rb").read()
            self._assets.append(data)
            L.emu_add_asset(b"rom.gb", data, len(data))
        if not L.emu_begin():
            sys.exit("the console would not start")
        self.name = L.emu_game_name().decode()
        self.touch = (False, 0, 0)
        self.held = 0
        self.turn = 0.0   # like a wheel: roll about the glass axis, what a game sees as turn
        self.pitch = 0.0  # top tipped toward you: the glass tips to face the ground
        self.yaw = 0.0    # turned about the world's vertical: what a game sees as looking around
        self.flat = False
        self.tilt = (0.0, 1.0, 0.0)   # upright, the way a watch is held to be turned like a wheel
        self._rot = None   # last orientation the gyro was derived from
        self._rot_last = (0.0, 0.0, 0.0)   # the frame rotation _gyro just derived, degrees
        # Fault injection: the synthetic sensor's own errors, so the calibration flow has
        # something to find. EMU_ACC_OFFSET=x,y,z (g), EMU_ACC_GAIN=x,y,z (what the part
        # reads for 1 g), EMU_GYRO_BIAS=x,y,z (deg/s) - comma-separated, applied to
        # everything the sensor reports, in the frame the firmware receives it.
        self.acc_off = Console._env3("EMU_ACC_OFFSET", 0.0)
        self.acc_gain = Console._env3("EMU_ACC_GAIN", 1.0)
        self.gyro_bias = Console._env3("EMU_GYRO_BIAS", 0.0)
        self._buf = ctypes.create_string_buffer(SCREEN * SCREEN * 3)
        self._lbuf = ctypes.create_string_buffer(1 << 16)
        self.log_lines = []   # every line the game has logged, drained by pump_log()
        self._log_shown = 0   # how many of them were already put on screen
        self.quiet = quiet
        self._abuf = (ctypes.c_int16 * 8192)()
        self.audio_rate = L.emu_audio_rate()
        self.audio = bytearray()   # every sample the game generated; a WAV at close()
        self.time = 0.0

    @staticmethod
    def _matmul(a, b):
        return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))

    def orientation(self, turn, pitch, yaw, flat):
        """The watch's orientation R (world -> body, 3x3). A vector fixed in the world reads
        in body axes as R @ v; with the watch held upright, world down is (0, 1, 0), which
        is where it starts. Yaw is about the world's vertical, so it never moves gravity -
        exactly like looking around on the spot - while turn (about the glass axis, the way
        the old pose() turned it) and pitch do. Lying flat is the same composition with the
        neutral pose tipped 90 degrees back."""
        t, p, y = map(math.radians, (turn, pitch + (90.0 if flat else 0.0), yaw))
        ct, st, cp, sp, cy, sy = math.cos(t), math.sin(t), math.cos(p), math.sin(p), math.cos(y), math.sin(y)
        mz = ((ct, st, 0.0), (-st, ct, 0.0), (0.0, 0.0, 1.0))   # turn, left-handed about z, so gravity keeps the old pose() formula
        mx = ((1.0, 0.0, 0.0), (0.0, cp, -sp), (0.0, sp, cp))   # pitch, right-handed about x
        my = ((cy, 0.0, sy), (0.0, 1.0, 0.0), (-sy, 0.0, cy))   # yaw, right-handed about y
        return self._matmul(mz, self._matmul(mx, my))

    def pose(self, turn_deg=None, pitch_deg=None, flat=None):
        """Set the pose angles (degrees; None keeps the current one) and derive gravity."""
        if turn_deg is not None:
            self.turn = turn_deg
        if pitch_deg is not None:
            self.pitch = pitch_deg
        if flat is not None:
            self.flat = flat
        r = self.orientation(self.turn, self.pitch, self.yaw, self.flat)
        self.tilt = (r[0][1], r[1][1], r[2][1])   # R @ (0,1,0): world down in body axes

    def _rot_delta(self):
        """Body-frame rotation over the last frame, in degrees, straight from how the
        orientation moved - the exact integral a real gyro only approximates. A scripted
        jump (`turn 90`) is one frame of very fast turning whose integral is the angle,
        which is what a filter needs to land upright at the new pose. The sign matters:
        R here is world->body, so the body-frame rotation is the rotation vector of
        R_prev^T @ R_new, which is the negative of the one R_new @ R_prev^T hands over."""
        r = self.orientation(self.turn, self.pitch, self.yaw, self.flat)
        if self._rot is None:
            self._rot = r
            return (0.0, 0.0, 0.0)
        d = [[sum(r[i][k] * self._rot[j][k] for k in range(3)) for j in range(3)] for i in range(3)]
        angle = math.acos(max(-1.0, min(1.0, (d[0][0] + d[1][1] + d[2][2] - 1.0) / 2.0)))
        self._rot = r
        if angle < 1e-9:
            return (0.0, 0.0, 0.0)
        # the axis from the skew part: (D32-D23, D13-D31, D21-D12), all row-major indices
        x, y, z = d[2][1] - d[1][2], d[0][2] - d[2][0], d[1][0] - d[0][1]
        n = math.sqrt(x * x + y * y + z * z)
        if n < 1e-9:
            return (0.0, 0.0, 0.0)
        s = -math.degrees(angle) / n
        return (x * s, y * s, z * s)

    def _gyro(self, dt):
        """Body turn rate from how the orientation moved since the last frame, the way an
        IMU would report it - the exact frame rotation of _rot_delta() divided by dt. The
        rotation itself stays in _rot_last for step() to hand the engine alongside."""
        self._rot_last = self._rot_delta()
        x, y, z = self._rot_last
        if dt <= 0:
            return (0.0, 0.0, 0.0)
        return (x / dt, y / dt, z / dt)

    @staticmethod
    def _env3(name, default):
        v = os.environ.get(name)
        if not v:
            return (default, default, default)
        parts = [float(p) for p in v.split(",")]
        if len(parts) != 3:
            sys.exit(f"{name} wants three comma-separated numbers")
        return tuple(parts)

    # The synthetic chip's mount - the one the real board's hardware sessions recorded
    # (components/engine's guessAxMap is the same map): per screen axis, the board axis
    # it lives on and its sign. screen x = -board y, screen y = -board x, screen z =
    # board z - a reflection, det -1, as every real mount must be: the chip's frame is
    # right-handed, the screen's (+x right, +y down, +z out of the glass) is left-handed.
    # The firmware's axis map undoes exactly this on the way back, so games and the
    # calibration flow see the screen-frame pose math above through the real code path -
    # the emulator once fed screen axes straight in with an identity map forced over
    # them, and rolling it walked gravity the OPPOSITE way to the watch.
    MOUNT = ((1, -1.0), (0, -1.0), (2, 1.0))

    def step(self, dt=1 / 60):
        down, x, y = self.touch
        gx, gy, gz = self._gyro(dt)
        rxryrz = self._rot_last   # what _gyro just derived, in degrees over dt
        # Into board axes, where a sensor lives its errors: gain, offset and bias apply
        # to what the part reports, and the firmware receives board-frame samples. The
        # integrated rotation rides along through the same mount - a real part's
        # integrated output carries its own bias over the interval, so the bias joins
        # the integral here.
        a = [0.0, 0.0, 0.0]
        g = [0.0, 0.0, 0.0]
        r = [0.0, 0.0, 0.0]
        screen = (self.tilt[0], self.tilt[1], self.tilt[2], gx, gy, gz)
        for i, (b, s) in enumerate(self.MOUNT):
            a[b] = s * screen[i]
            # The rate and its integral reach games with screen y mirrored: heading math
            # is locked against this emulator's yaw script (positive yaw = a physical
            # turn LEFT), and hardware A/B pinned the split - pitch (x) and roll (z)
            # pass through true, yaw arrives
            # inverted. applyPipeline mirrors screen y out of the board data, so the front
            # end mirrors it here first and the round trip still hands the game the exact
            # pose rotation it locked its signs against.
            flip = -1.0 if i == 1 else 1.0
            g[b] = flip * s * screen[3 + i]
            r[b] = flip * s * rxryrz[i]
        ax = a[0] * self.acc_gain[0] + self.acc_off[0]
        ay = a[1] * self.acc_gain[1] + self.acc_off[1]
        az = a[2] * self.acc_gain[2] + self.acc_off[2]
        self.lib.emu_input(1 if down else 0, int(x), int(y), self.held, ax, ay, az,
                           g[0] + self.gyro_bias[0], g[1] + self.gyro_bias[1], g[2] + self.gyro_bias[2],
                           r[0] + self.gyro_bias[0] * dt, r[1] + self.gyro_bias[1] * dt,
                           r[2] + self.gyro_bias[2] * dt, dt)
        self.time += dt
        return self.lib.emu_step(dt) != 0

    def run(self, seconds, dt=1 / 60):
        home = False
        for _ in range(max(1, round(seconds / dt))):
            home = self.step(dt) or home
        return home

    def state_save(self):
        """The game's sleep snapshot: bytes the console would write to /data/.sleep."""
        buf = ctypes.create_string_buffer(32 * 1024)
        n = self.lib.emu_state_save(buf, len(buf))
        return buf.raw[:n] if n > 0 else b""

    def state_load(self, blob):
        return bool(self.lib.emu_state_load(blob, len(blob)))

    def frame(self, corner=(0, 0, 0)):
        self.lib.emu_frame(self._buf, *corner)
        return self._buf.raw

    def image(self, corner=(0, 0, 0)):
        from PIL import Image
        return Image.frombytes("RGB", (SCREEN, SCREEN), self.frame(corner))

    def pump_audio(self):
        """Move whatever PCM the game has generated into the Python-side pile."""
        while True:
            n = self.lib.emu_audio_take(self._abuf, len(self._abuf))
            if n <= 0:
                return
            self.audio += ctypes.string_at(self._abuf, n * 2)

    def pump_log(self):
        """Move whatever the game has logged since the last call into log_lines - the
        material wait_log matches against. The library piles lines up until taken,
        exactly like its PCM stream."""
        while True:
            n = self.lib.emu_log_take(self._lbuf, len(self._lbuf))
            if n <= 0:
                return
            self.log_lines += self._lbuf.raw[:n].decode("utf-8", errors="replace").splitlines()

    def dump_log_tail(self):
        """A run died (a wait timed out, sys.exit in a script): print what the game logged
        since it was last shown - the trail leading to the failure. Under --quiet those
        lines went nowhere else, so give the last of them all."""
        self.pump_log()
        new = self.log_lines[self._log_shown:]
        self._log_shown = len(self.log_lines)
        if self.quiet:
            new = self.log_lines[-20:]
        for l in new:
            print(l)

    def close(self):
        self.pump_audio()
        if self.audio:
            import wave
            path = os.path.join(OUT, self.game, "audio.wav")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with wave.open(path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(self.audio_rate)
                w.writeframes(bytes(self.audio))
            print(f"  audio: {len(self.audio) // 2 / self.audio_rate:.1f} s -> {os.path.relpath(path, ROOT)}")
        self.lib.emu_end()


# ---------------------------------------------------------------------------- scripted

def run_script(console, script, out_dir):
    from PIL import Image
    shots = []
    steps = [s.strip() for s in script.replace("\n", ";").split(";") if s.strip() and not s.strip().startswith("#")]
    turn = pitch = 0.0
    for step in steps:
        op, *a = step.split()
        if op == "wait":
            console.run(float(a[0]))
        elif op == "tap":
            console.touch = (True, float(a[0]), float(a[1]))
            console.run(0.08)
            console.touch = (False, float(a[0]), float(a[1]))
            console.run(0.05)
        elif op == "touch":
            console.touch = (True, float(a[0]), float(a[1]))
            console.run(1 / 60)
        elif op == "release":
            console.touch = (False, console.touch[1], console.touch[2])
            console.run(1 / 60)
        elif op == "swipe":
            x0, y0, x1, y1 = map(float, a[:4])
            n = max(2, round((float(a[4]) if len(a) > 4 else 0.2) * 60))
            for i in range(n + 1):
                console.touch = (True, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n)
                console.step()
            console.touch = (False, x1, y1)
            console.run(0.05)
        elif op == "tilt":
            console.tilt = tuple(map(float, a[:3]))
        elif op == "turn":
            turn = float(a[0])
            console.pose(turn, pitch)
        elif op == "pitch":
            pitch = float(a[0])
            console.pose(turn, pitch)
        elif op == "yaw":   # look around the vertical axis; gravity does not move
            console.yaw = float(a[0])
            console.pose()
        elif op == "flat":  # lying on the table (1) or held (0); pitch stacks on top
            console.flat = a[0] not in ("0", "false")
            console.pose()
        elif op == "button":
            console.held = BTN_B
            console.run(float(a[1]) if len(a) > 1 else 0.1)
            console.held = 0
            console.run(0.05)
        elif op == "hold":      # PWR down, and left down: `hold b; wait 2; letgo`
            console.held = BTN_B
            console.run(1 / 60)
        elif op == "letgo":
            console.held = 0
            console.run(0.05)
        elif op == "wait_pixel":
            # Frames until the pixel lands on the color (within tol): a script keyed to
            # what is on screen instead of to one seed's timings - hardcoded waits are
            # fragile across seeds and machines.
            x, y, want = int(a[0]), int(a[1]), a[2].lstrip("#")
            tol = int(a[3]) if len(a) > 3 else 8
            limit = float(a[4]) if len(a) > 4 else 10.0
            if len(want) != 6:
                sys.exit(f"wait_pixel: {a[2]!r} is not a #rrggbb color")
            target = tuple(int(want[i:i + 2], 16) for i in (0, 2, 4))
            end = console.time + limit
            while True:
                px = console.image(corner=(14, 14, 28)).getpixel((x, y))
                if all(abs(px[i] - target[i]) <= tol for i in range(3)):
                    break
                if console.time >= end:
                    sys.exit(f"wait_pixel: ({x}, {y}) never reached #{want} within {limit}s "
                             f"(last {px}; is the coordinate inside the visible circle?)")
                console.step()
        elif op == "wait_log":
            # Frames until the game has logged a matching line (T->log output, drained
            # from the library's pile even under --quiet). The game's own telemetry -
            # its debug defines - is what makes aim/tilt checks assertable at all.
            pattern = a[0].strip("\"'")
            limit = float(a[1]) if len(a) > 1 else 30.0
            try:
                rx = re.compile(pattern)
            except re.error as e:
                sys.exit(f"wait_log: bad regex {pattern!r}: {e}")
            end = console.time + limit
            console.pump_log()
            while not any(rx.search(l) for l in console.log_lines):
                if console.time >= end:
                    sys.exit(f"wait_log: no line matching {pattern!r} within {limit}s")
                console.step()
                console.pump_log()
        elif op == "shot":
            path = a[0] if os.path.isabs(a[0]) else os.path.join(out_dir, a[0])
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            img = console.image(corner=(14, 14, 28))
            img.save(path)
            shots.append(img)
            print(f"  {console.time:6.2f} s  {os.path.relpath(path, ROOT)}")
        elif op == "sheet":
            path = a[0] if os.path.isabs(a[0]) else os.path.join(out_dir, a[0])
            cols = min(4, len(shots)) or 1
            rows = (len(shots) + cols - 1) // cols
            cell = 312
            sheet = Image.new("RGB", (cols * cell, rows * cell), (14, 14, 28))
            for i, img in enumerate(shots):
                sheet.paste(img.resize((cell - 2, cell - 2), Image.NEAREST), ((i % cols) * cell + 1, (i // cols) * cell + 1))
            sheet.save(path)
            print(f"  sheet of {len(shots)}: {os.path.relpath(path, ROOT)}")
        else:
            sys.exit(f"don't know how to '{step}'")


# ---------------------------------------------------------------------------- the window

def play(console, zoom=1.0, record=None):
    """A window to play in. With `record`, what is played is also filmed: every frame, pixel for
    pixel at 2x on a black 1080 square, at exactly sixty frames a second of the game's own time -
    so the film is smooth even where the window was not."""
    import tkinter as tk
    from PIL import Image, ImageTk

    # A game's own render goes out live when sounddevice is installed; either way the whole
    # session lands in build/emu/<game>/audio.wav at close().
    speaker = None
    try:
        import sounddevice
        speaker = sounddevice.OutputStream(samplerate=console.audio_rate, channels=1, dtype="int16")
        speaker.start()
        print("audio: live (sounddevice)")
    except Exception:
        print("audio: silent (no sounddevice; the WAV is written at exit)")
    played = 0   # how much of console.audio has gone to the speaker already

    size = int(SCREEN * zoom)
    root = tk.Tk()
    root.title(f"Gyro-Tron - {console.name}")
    root.configure(bg="#0e0e1c")
    root.resizable(False, False)
    label = tk.Label(root, bd=0, bg="#0e0e1c", cursor="hand2")
    label.pack(padx=18, pady=(18, 6))
    status = tk.Label(root, bg="#0e0e1c", fg="#8a97c0", font=("Consolas", 9))
    status.pack(pady=(0, 10))

    film = None
    if record:
        if not shutil.which("ffmpeg"):
            sys.exit("recording needs ffmpeg on the PATH")
        os.makedirs(os.path.dirname(os.path.abspath(record)) or ".", exist_ok=True)
        film = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "1080x1080", "-r", "60",
             "-i", "-", "-c:v", "libx264", "-preset", "fast", "-crf", "12", "-pix_fmt", "yuv420p", "-movflags",
             "+faststart", record], stdin=subprocess.PIPE)

    state = {"turn": 0.0, "pitch": 0.0, "yaw": 0.0, "flat": False, "keys": set(), "quit": False, "photo": None, "last": time.time(), "shot": 0}

    def touch(down):
        def handler(e):
            console.touch = (down, e.x / zoom, e.y / zoom)
        return handler

    label.bind("<ButtonPress-1>", touch(True))
    label.bind("<B1-Motion>", touch(True))
    label.bind("<ButtonRelease-1>", touch(False))

    def key_down(e):
        k = e.keysym.lower()
        state["keys"].add(k)
        if k == "space":
            state["turn"] = state["pitch"] = 0.0
        elif k == "f":
            state["flat"] = not state["flat"]
        elif k in ("escape", "h"):
            state["quit"] = True
        elif k == "p":
            state["shot"] += 1
            path = os.path.join(OUT, console.game, f"shot-{state['shot']:02d}.png")
            console.image(corner=(14, 14, 28)).save(path)
            print("saved", os.path.relpath(path, ROOT))

    root.bind("<KeyPress>", key_down)
    root.bind("<KeyRelease>", lambda e: state["keys"].discard(e.keysym.lower()))

    def frame():
        now = time.time()
        dt = 1 / 60 if film else min(0.05, now - state["last"])
        state["last"] = now
        keys = state["keys"]
        rate = (25.0 if ("shift_l" in keys or "shift_r" in keys) else 90.0) * dt
        if "left" in keys: state["turn"] -= rate
        if "right" in keys: state["turn"] += rate
        if "up" in keys: state["pitch"] += rate
        if "down" in keys: state["pitch"] -= rate
        if "a" in keys: state["yaw"] -= rate
        if "d" in keys: state["yaw"] += rate
        state["turn"] = max(-180.0, min(180.0, state["turn"]))
        state["pitch"] = max(-80.0, min(80.0, state["pitch"]))
        console.yaw = state["yaw"]
        console.pose(state["turn"], state["pitch"], state["flat"])
        console.held = BTN_B if "b" in keys else 0

        if console.step(dt) or state["quit"]:
            if film:
                film.stdin.close()
                film.wait()
                print("filmed", record)
            console.close()
            root.destroy()
            return
        console.pump_audio()
        if speaker is not None:
            # write() blocks until the chunk is consumed, which is the pacing: the game
            # makes exactly as many samples per step as this loop spends playing them.
            speaker.write(memoryview(console.audio)[played:])
            played = len(console.audio)
        if film:
            shot = Image.new("RGB", (1080, 1080), (0, 0, 0))
            shot.paste(Image.frombytes("RGB", (SCREEN, SCREEN), console.frame(corner=(0, 0, 0))).resize((932, 932), Image.NEAREST),
                       (74, 74))
            film.stdin.write(shot.tobytes())
        img = Image.frombytes("RGB", (SCREEN, SCREEN), console.frame(corner=(14, 14, 28)))
        if zoom != 1.0:
            img = img.resize((size, size), Image.NEAREST)
        state["photo"] = ImageTk.PhotoImage(img)
        label.configure(image=state["photo"])
        status.configure(text=f"{'flat' if state['flat'] else 'upright'}   turn {state['turn']:+4.0f}   pitch {state['pitch']:+4.0f}   yaw {state['yaw']:+5.0f}"
                              "     arrows tilt - A/D look around - space level - F flat - B pwr - P picture - Esc quit")
        root.after(8, frame)

    root.after(0, frame)
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description="run a Gyro-Tron game on a PC", epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("play", help="a window to play it in")
    p.add_argument("game")
    p.add_argument("--zoom", type=float, default=1.0)
    p.add_argument("--fresh", action="store_true", help="forget the saved scores first")
    p.add_argument("--record", metavar="FILE.mp4", help="film what is played (needs ffmpeg)")
    p.add_argument("--define", action="append", default=[], metavar="NAME",
                   help="build the game with this preprocessor symbol (repeatable)")
    s = sub.add_parser("shot", help="run a script and save pictures")
    s.add_argument("game")
    s.add_argument("script", help="the steps, or a file of them")
    s.add_argument("--out", default=None, help="where pictures go (default build/emu/<game>)")
    s.add_argument("--seed", type=int, default=1, help="the dice; the same seed is the same run")
    s.add_argument("--fresh", action="store_true", help="forget the saved scores first")
    s.add_argument("--quiet", action="store_true", help="no log")
    s.add_argument("--asset", action="append", default=[], metavar="NAME=PATH",
                   help="stand in a file for one the game packages (repeatable)")
    s.add_argument("--define", action="append", default=[], metavar="NAME",
                   help="build the game with this preprocessor symbol (repeatable)")
    b = sub.add_parser("build", help="just build it")
    b.add_argument("game", nargs="+")
    l = sub.add_parser("load", help="load a .tat the way the watch does - the real loader, "
                                    "no execution")
    l.add_argument("package", help="a .tat file, or a game id with a build/<id>.tat")
    args = ap.parse_args()

    if args.cmd == "build":
        for g in args.game:
            build(g, force=True, verbose=True)
        return
    if args.cmd == "load":
        p = args.package
        if not os.path.exists(p) and not os.path.splitext(p)[1]:
            p = os.path.join(ROOT, "build", p + ".tat")   # a game id: what mktat installs
        if not os.path.exists(p):
            sys.exit(f"no such package: {args.package}"
                     + ("" if os.path.splitext(args.package)[1]
                        else f" - pack one with python tools/mktat.py games/{args.package}"))
        sys.exit(load_tat(p))
    if args.cmd == "play":
        play(Console(args.game, fresh=args.fresh, verbose=True, defines=tuple(args.define)),
             args.zoom, args.record)
        return
    script = open(args.script).read() if os.path.exists(args.script) else args.script
    assets = [a.split("=", 1) for a in args.asset]
    console = Console(args.game, seed=args.seed, quiet=args.quiet, fresh=args.fresh, verbose=True,
                      assets=assets, defines=tuple(args.define))
    try:
        run_script(console, script, args.out or os.path.join(OUT, args.game))
    finally:
        console.dump_log_tail()
    console.close()


if __name__ == "__main__":
    main()
