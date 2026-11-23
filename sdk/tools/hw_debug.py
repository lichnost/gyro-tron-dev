"""Drive a real Gyro-Tron the way tools/emu drives the emulator.

The other half of remote debugging: the watch runs the same script words
(tools/emu/README.md), answers screenshots in fast formats and takes them over
Wi-Fi or USB. docs/reference/remote_debug.md is the spec.

    python tools/hw_debug.py --host gyro-tron.local --apps
    python tools/hw_debug.py --host 192.168.1.20 --game tiltatris \\
        --script "wait 1; tap 233 380; wait 2; shot"
    python tools/hw_debug.py --port COM4 --shot menu.png

Wi-Fi is the fast path: screenshots in raw RGB565, or the delta format that only
sends what changed, and scripts that run on the watch itself. USB works without a
network - shots go through the link's own commands and the script is interpreted
here, step by step.

Opening the USB port must not toggle DTR/RTS: that resets the chip (tatlink.py).
"""
import argparse
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tatlink

FRAME_BYTES = 466 * 466 * 2
SETTLE_S = 0.23   # what the watch's own script interpreter adds after an input step


def resolve_app(apps, word):
    """A number stays a number; anything else is an id, looked up in the carousel as it
    is now. A stale `app 13` launched the wrong game after a reinstall reordered it."""
    if word.isdigit():
        return int(word)
    exact = [i for i, gid, _ in apps if gid.lower() == word.lower()]
    hits = exact or [i for i, gid, _ in apps if word.lower() in gid.lower()]
    if not hits:
        raise ValueError(f"no app matches {word!r}: " + ", ".join(f"{i}:{gid}" for i, gid, _ in apps))
    return hits[0]


def wait_log(watch, pattern, seconds=30.0):
    """Poll the watch's log until a line NEW since this call matches; the ring still holds
    old boot lines, and matching those would make every sync instant and wrong."""
    rx = re.compile(pattern)
    seen = set(watch.log(tail=1 << 20).splitlines())
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for line in watch.log(tail=1 << 20).splitlines():
            if line not in seen and rx.search(line):
                return line
        time.sleep(0.5)
    raise TimeoutError(f"waitlog timed out after {seconds:.0f}s: {pattern}")


def parse_waitlog(step):
    """`waitlog "<regex>" [seconds]`, quotes optional for a regex without spaces."""
    m = re.fullmatch(r'waitlog\s+(?:"([^"]*)"|(\S+))(?:\s+([\d.]+))?', step.strip())
    if not m:
        raise ValueError('waitlog wants: waitlog "<regex>" [seconds]')
    return m.group(1) if m.group(1) is not None else m.group(2), float(m.group(3) or 30)


def rgb565_to_png(w, h, data):
    """A frame of RGB565 (little-endian) as a PIL image, for saving as PNG."""
    try:
        import numpy as np
        px = np.frombuffer(data, dtype="<u2").reshape(h, w)
        rgb = np.stack([((px >> 11) & 31) << 3, ((px >> 5) & 63) << 2, (px & 31) << 3], -1)
        from PIL import Image
        return Image.fromarray(rgb.astype("uint8"), "RGB")
    except ImportError:
        from PIL import Image
        img = Image.new("RGB", (w, h))
        px = img.load()
        for y in range(h):
            for x in range(w):
                v = int.from_bytes(data[(y * w + x) * 2:(y * w + x) * 2 + 2], "little")
                px[x, y] = (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
        return img


class WifiWatch:
    """Over HTTP: /screen, /input, /apps, /script, /log."""

    def __init__(self, host, key, timeout=300):
        self.host = host
        self.key = key
        self.timeout = timeout
        self._prev = None   # the last delta frame, for decoding the next one

    def _get(self, path, timeout=None):
        url = f"http://{self.host}{path}"
        if self.key:
            url += ("&" if "?" in url else "?") + f"key={self.key}"
        return urllib.request.urlopen(url, timeout=timeout or self.timeout).read()

    def status(self):
        return json.loads(self._get("/status"))

    def apps(self):
        return [(a["i"], a["id"], a["name"]) for a in json.loads(self._get("/apps"))]

    def input(self, query):
        self._get(f"/input?{query}")

    def _apply_delta(self, data):
        """Decode a /screen answer: GTD1 delta against the previous one, or raw."""
        if data[:4] != b"GTD1":
            self._prev = bytearray(data)   # raw: the whole frame, unchanged since 0.9.x
            return 466, 466, bytes(self._prev)
        w, h, rects = struct.unpack_from("<HHH", data, 4)
        at = 12
        if self._prev is None or len(self._prev) != w * h * 2:
            self._prev = bytearray(w * h * 2)
        for _ in range(rects):
            y, rows, enc, _pad = struct.unpack_from("<HHBB", data, at)
            at += 6
            count = rows * w
            if enc == 0:
                self._prev[y * w * 2:(y + rows) * w * 2] = data[at:at + count * 2]
                at += count * 2
            else:
                out, base = 0, y * w
                while out < count:
                    run, val = struct.unpack_from("<HH", data, at)
                    at += 4
                    self._prev[(base + out) * 2:(base + out + run) * 2] = struct.pack("<H", val)
                    out += run
        return w, h, bytes(self._prev)

    def shot(self, fmt="delta"):
        return self._apply_delta(self._get(f"/screen?fmt={fmt}"))

    def log(self, tail=4000):
        return self._get("/log")[-tail:].decode(errors="replace")

    def script(self, body, on_note, on_shot):
        """The steps run on the watch, except the two words only this side can answer:
        `app <id>` is resolved here and `waitlog` splits the script around a log poll."""
        chunk = []
        for step in filter(None, (s.strip() for s in re.split(r"[;\n]", body))):
            words = step.split()
            if words[0] == "app" and len(words) > 1:
                step = f"app {resolve_app(self.apps(), words[1])}"
            if words[0] != "waitlog":
                chunk.append(step)
                continue
            if chunk and not self._run_on_watch("; ".join(chunk), on_note, on_shot):
                return False
            chunk = []
            try:
                on_note(f"{step} -> {wait_log(self, *parse_waitlog(step))}")
            except (TimeoutError, ValueError) as e:
                print(f"  ERROR: {e}", flush=True)
                return False
        return self._run_on_watch("; ".join(chunk), on_note, on_shot) if chunk else True

    def _run_on_watch(self, body, on_note, on_shot):
        """POST /script: records stream back as the steps run."""
        url = f"http://{self.host}/script"
        if self.key:
            url += f"?key={self.key}"
        req = urllib.request.Request(url, data=body.encode(), method="POST")
        resp = urllib.request.urlopen(req, timeout=self.timeout)
        ok, buf = None, b""
        while True:
            chunk = resp.read(65536)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 5:
                kind = buf[0]
                ln = int.from_bytes(buf[1:5], "little")
                if len(buf) < 5 + ln:
                    break
                data, buf = buf[5:5 + ln], buf[5 + ln:]
                if kind == 1:
                    on_shot(data)
                elif kind == 2:
                    on_note(data.decode(errors="replace"))
                elif kind == 3:
                    print(f"  ERROR: {data.decode(errors='replace')}", flush=True)
                    ok = False
                elif kind == 0:
                    ok = data == b"ok"
        return bool(ok)


class UsbWatch:
    """Over the link protocol: shots and input; the script is interpreted right here."""

    def __init__(self, port):
        self.w = tatlink.Watch(port, timeout=5.0)

    def hello(self):
        return self.w.hello()

    def apps(self):
        return self.w.apps()

    def input(self, query):
        self.w.input(query)

    def shot(self, fmt=None):
        return self.w.shot()   # (w, h, RGB565 little-endian)

    def log(self, tail=4000):
        raw = self.w.log().decode("utf-8", errors="replace")
        return raw[-tail:]

    def _waitpx(self, words):
        x, y = int(words[1]), int(words[2])
        tol = int(words[4]) if len(words) > 4 else 8
        deadline = time.monotonic() + (float(words[5]) if len(words) > 5 else 10)
        rgb = int(words[3].lstrip("#"), 16)
        while time.monotonic() < deadline:
            _, _, data = self.shot()
            o = (y * 466 + x) * 2
            v = int.from_bytes(data[o:o + 2], "little")
            got = (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
            if all(abs(a - b) <= tol for a, b in
                   zip(got, ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255))):
                return True
            time.sleep(1.0)
        raise TimeoutError(f"waitpx timed out at {x},{y}")

    def script(self, body, on_note, on_shot):
        ok = True
        for step in re.split(r"[;\n]", body):
            step = step.strip()
            if not step:
                continue
            t0 = time.monotonic()
            words = step.split()
            try:
                w = words[0]
                if w == "wait":
                    time.sleep(float(words[1]))
                elif w == "tap":
                    self.input(f"tap={words[1]},{words[2]}")
                    time.sleep(SETTLE_S)
                elif w == "hold":
                    ms = int(words[3])
                    self.input(f"hold={words[1]},{words[2]},{ms}")
                    time.sleep(ms / 1000 + SETTLE_S)
                elif w == "swipe":
                    if len(words) > 5 and float(words[5]) != 0.16:
                        on_note(f"note: a USB swipe always runs 160 ms ({step})")
                    self.input(f"swipe={words[1]},{words[2]},{words[3]},{words[4]}")
                    time.sleep(SETTLE_S)
                elif w == "btn":
                    sec = float(words[2]) if len(words) > 2 else 0.08
                    self.input(f"btn={words[1]},{int(sec * 1000)}")
                    time.sleep(sec + SETTLE_S)
                elif w == "tilt":
                    self.input(f"tilt={words[1]},{words[2]},{words[3]}")
                elif w == "tiltoff":
                    self.input("tilt=off")
                elif w == "app":
                    self.input(f"app={resolve_app(self.apps(), words[1])}")
                elif w == "waitlog":
                    on_note(f"  matched: {wait_log(self, *parse_waitlog(step))}")
                elif w == "home":
                    self.input("app=0")
                elif w == "sleep":
                    self.input("sleep=1")
                elif w == "wake":
                    self.input("wake=1")
                elif w == "shot":
                    on_shot(self.shot()[2])
                elif w == "waitpx":
                    self._waitpx(words)
                else:
                    raise ValueError(f"unknown step {w!r}")
                on_note(f"{step} -> ok ({time.monotonic() - t0:.1f}s)")
            except Exception as e:   # noqa: BLE001 - one bad step ends the script
                print(f"  ERROR: {step}: {e}", flush=True)
                on_note(f"{step} -> FAIL ({time.monotonic() - t0:.1f}s)")
                return False
        return ok


def serial_capture(port, query, seconds):
    """Send one INPUT, then print the raw UART for `seconds`: log lines, panics and
    backtraces, which idf.py monitor shows but cannot while the link holds the port.
    A panic resets the chip and the USB port drops with it, so a lost port is reopened."""
    w = tatlink.Watch(port)
    if query:
        w.input(query)
    ser = w.ser
    deadline = time.monotonic() + seconds
    buf = b""
    while time.monotonic() < deadline:
        try:
            buf += ser.read(4096)
        except Exception:   # noqa: BLE001 - pyserial's error for a vanished port varies
            ser.close()
            print("--- port dropped (reset?) ---", flush=True)
            while time.monotonic() < deadline:
                time.sleep(0.3)
                try:
                    ser = tatlink.open_port(port)
                    break
                except Exception:   # noqa: BLE001
                    continue
            continue
        *lines, buf = buf.split(b"\n")
        for line in lines:
            # Link frames are binary and share the port; errors="ignore" drops their bytes.
            text = line.decode("utf-8", errors="ignore").rstrip()
            if text:
                print(text, flush=True)
    ser.close()


def install(watch, path):
    """The manager CLI does the install - never a second implementation. Its output is
    captured because the GUI-subsystem binary otherwise loses it on Windows."""
    if not os.path.getsize(path):
        sys.exit(f"{path} is empty")
    exe = shutil.which("gyrotron-manager")
    cmd = [exe] if exe else ["dotnet", "run", "--project", "app/GyroTron.Manager", "--"]
    watch.w.close()   # the manager opens the port itself, and only one side can hold it
    r = subprocess.run(cmd + ["install", path], capture_output=True, text=True, errors="replace")
    print(r.stdout + r.stderr, end="", flush=True)
    if r.returncode:
        sys.exit(f"install failed ({r.returncode})")


def verify_installed(watch, path):
    """The manager stores a package as Games/<id>.tat and mktat names the file after the
    package id, so the names line up - this guards against a truncated or vanished file
    that a failed install leaves unnoticed."""
    size = os.path.getsize(path)
    name = os.path.basename(path).lower()
    stored = [e for e in watch.w.fs_list("Games") if e["name"].lower() == name]
    if not stored:
        sys.exit(f"Games/{name} is not on the watch after the install")
    if stored[0]["size"] != size:
        sys.exit(f"Games/{name} is {stored[0]['size']} bytes on the watch, expected {size}")
    print(f"verified: Games/{name}, {size} bytes")


def doctor(host):
    """Which leg of the Wi-Fi path is broken: interface, name, ARP, TCP, HTTP."""
    win = os.name == "nt"
    cmd = ["netsh", "wlan", "show", "interfaces"] if win else ["ip", "-br", "addr"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=10).stdout
        print("[interface]\n" + "\n".join("  " + l.strip() for l in out.splitlines() if l.strip())[:800])
    except (OSError, subprocess.TimeoutExpired) as e:
        print(f"[interface] could not ask: {e}")
    if not host:
        print("no --host: stopping at the interface")
        return
    try:
        ip = socket.gethostbyname(host)
        print(f"[name] {host} -> {ip}")
    except OSError as e:
        print(f"[name] BROKEN: {e} - no mDNS here? give the IP instead")
        return
    subprocess.run(["ping", "-n" if win else "-c", "1", ip], capture_output=True, timeout=10)
    arp = subprocess.run(["arp", "-a"], capture_output=True, text=True, errors="replace", timeout=10).stdout
    has_arp = any(ip in line for line in arp.splitlines())
    print(f"[arp] {'present' if has_arp else 'MISSING - the watch is not on this segment, or asleep'}")
    try:
        socket.create_connection((ip, 80), timeout=3).close()
        print("[tcp] port 80 open")
    except OSError as e:
        hint = " - ARP answers, TCP does not: suspect client isolation or a firewall" if has_arp else ""
        print(f"[tcp] BROKEN: {e}{hint}")
        return
    try:
        body = urllib.request.urlopen(f"http://{ip}/status", timeout=5).read(200)
        print("[http] /status " + body.decode(errors="replace"))
    except Exception as e:   # noqa: BLE001 - urllib raises half a dozen kinds here
        print(f"[http] {e} (a 401 means the path is fine and only the key is missing)")


def open_watch(args):
    """Wi-Fi when --host is given, USB when --port is. A --host without --key
    borrows the key from the watch over USB, where physical access implies trust."""
    if args.host:
        key = args.key
        if not key:
            for port in ([args.port] if args.port else tatlink.find_watch()):
                try:
                    key = tatlink.Watch(port).hello().get("key")
                    if key:
                        break
                except Exception:
                    pass
            if not key:
                sys.exit("no --key and the USB port did not hand one over; "
                         "the watch shows it in Settings > UPDATE")
        return WifiWatch(args.host, key)
    ports = [args.port] if args.port else tatlink.find_watch()
    last = None
    for port in ports:
        try:
            return UsbWatch(port)
        except Exception as e:
            last = e
    sys.exit(f"couldn't reach a watch over USB. last error - {last}")


def save_shot(out_dir, index, w, h, data):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"shot-{index:02d}.png")
    try:
        rgb565_to_png(w, h, data).save(path)
    except ImportError:   # no Pillow: keep the frame anyway
        path += ".raw"
        open(path, "wb").write(data)
    print(f"  shot -> {path} ({w}x{h}, {len(data)} bytes)", flush=True)
    return path


def main():
    ap = argparse.ArgumentParser(description="Debug a Gyro-Tron over Wi-Fi or USB")
    ap.add_argument("--host", help="the watch's IP or gyro-tron.local (Wi-Fi)")
    ap.add_argument("--key", help="the watch's Wi-Fi key (fetched over USB if omitted)")
    ap.add_argument("--port", help="serial port (USB; default: auto-detect)")
    ap.add_argument("--apps", action="store_true", help="list the carousel with app numbers")
    ap.add_argument("--game", metavar="ID", help="launch this game first (resolved via /apps)")
    ap.add_argument("--script", metavar="STEPS", help='run steps, e.g. "wait 1; tap 233 380; shot"')
    ap.add_argument("--shot", metavar="FILE.png", help="take one screenshot into FILE")
    ap.add_argument("--fmt", default="delta", choices=("png", "raw", "delta"),
                    help="screenshot format for --shot (Wi-Fi); default delta")
    ap.add_argument("--log", action="store_true", help="print the tail of the watch's log (Wi-Fi or USB)")
    ap.add_argument("--out", default="build/hw-debug", help="where script shots land")
    ap.add_argument("--install", metavar="FILE.tat", help="install via the manager CLI, verify the stored size (USB)")
    ap.add_argument("--rescan", action="store_true", help="make the launcher re-read the games folder")
    ap.add_argument("--reboot", action="store_true", help="restart the console (runs last)")
    ap.add_argument("--retry", type=int, default=3, help="--game: attempts, re-resolving the carousel each time")
    ap.add_argument("--serial", metavar="QUERY", nargs="?", const="",
                    help='send one input ("app=3", or nothing), then print raw serial; USB only')
    ap.add_argument("--seconds", type=float, default=20, help="how long --serial listens")
    ap.add_argument("--doctor", action="store_true", help="say which leg of the Wi-Fi path is broken")
    args = ap.parse_args()
    if not (args.host or args.port or args.shot or args.script or args.apps or args.game or args.log
            or args.install or args.rescan or args.reboot or args.serial is not None or args.doctor):
        ap.error("nothing to do: give --host or --port, and --shot, --script or --apps")

    if args.doctor:
        doctor(args.host)
        return
    if args.serial is not None:
        ports = [args.port] if args.port else tatlink.find_watch()
        if not ports:
            sys.exit("no USB port found")
        serial_capture(ports[0], args.serial, args.seconds)
        return

    watch = open_watch(args)
    if args.host:
        status = watch.status()
        print(f"Gyro-Tron {status.get('version')} at {args.host} "
              f"(psram largest {status.get('psram_largest', 0) // 1024} KB)")
    else:
        hi = watch.hello()
        print(f"Gyro-Tron on USB: firmware {hi['firmware']}, link protocol {hi['proto']}")

    if args.install:
        if args.host:
            sys.exit("--install works over USB")
        install(watch, args.install)
        watch = open_watch(args)
        verify_installed(watch, args.install)

    if args.rescan:
        watch.input("rescan=1")
        print("rescan asked for; the launcher picks it up on the home screen")

    if args.game:
        attempts = max(args.retry, 1)
        for attempt in range(attempts):
            try:
                index = resolve_app(watch.apps(), args.game)
                print(f"launching {args.game} (app {index})")
                watch.input(f"app={index}")
                break
            except Exception as e:   # noqa: BLE001 - HTTP and link errors both land here
                if attempt == attempts - 1:
                    sys.exit(f"couldn't launch {args.game}: {e}")
                time.sleep(1.0)
        time.sleep(1.0)

    if args.apps:
        for i, gid, name in watch.apps():
            print(f"  app {i:<3} {gid:<16} {name}")

    if args.shot:
        w, h, data = watch.shot(args.fmt if args.host else None)
        os.makedirs(os.path.dirname(os.path.abspath(args.shot)), exist_ok=True)
        rgb565_to_png(w, h, data).save(args.shot)
        print(f"wrote {args.shot} ({w}x{h}, {len(data)} bytes"
              + (f", {args.fmt}" if args.host else ", USB raw") + ")")

    if args.script:
        counter = [0]

        def on_shot(data):
            counter[0] += 1
            save_shot(args.out, counter[0], 466, 466, data)

        t0 = time.monotonic()
        ok = watch.script(args.script, lambda s: print(f"  {s}", flush=True), on_shot)
        print(f"script {'ok' if ok else 'FAILED'} in {time.monotonic() - t0:.1f}s")

    if args.log:
        print("\n--- log tail ---")
        print(watch.log())

    if args.reboot:
        watch.input("reboot=1")
        print("rebooting")


if __name__ == "__main__":
    main()
