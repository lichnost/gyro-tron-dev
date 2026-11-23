"""Self-test for the device tooling in server.py - run it after touching
server.py or tatref.py:

    python mcp/selftest.py            (plugins/gyro-tron-dev/mcp/ in the firmware repo)

Layers:

1. Offline - golden-image comparison (the assertion layer for screenshot
   suites), the frozen export list mktat checks the ELF against (pure parsing;
   the live nm run is exercised by every build_package), and the doc-vs-header
   drift: GAME_API.md is a design spec that runs ahead of the header, and
   duckhunt once wrote sheet_free calls against it that the watch refused.
   No GPU, no toolchain.
2. MCP protocol - speaks JSON-RPC to server.py over stdio the way the host
   does: initialize, the exact tool list, and create_game (the placeholder
   icon within its 4096-byte budget).

Artifacts land in build/selftest/ under the root and a scratch game games/selftest_tmp is
removed again; the run leaves nothing behind on success or failure (finally
blocks).
"""

import json
import os
import queue
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The firmware repo (plugins/gyro-tron-dev/mcp -> repo), the standalone public repo
# (mcp's parent, with the exported tree under sdk/), or GYROTRON_ROOT. The same
# selftest is the acceptance gate in both.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
if not os.path.isfile(os.path.join(ROOT, "tools", "mktat.py")):
    ROOT = os.path.dirname(HERE)
    if not os.path.isfile(os.path.join(ROOT, "tools", "mktat.py")):
        ROOT = os.path.join(ROOT, "sdk")
    if not os.path.isfile(os.path.join(ROOT, "tools", "mktat.py")):
        ROOT = os.environ.get("GYROTRON_ROOT", ROOT)

FAILURES = []


def check(name, cond, detail=""):
    mark = "PASS" if cond else "FAIL"
    print(f"  [{mark}] {name}" + (f" - {detail}" if detail and not cond else ""))
    if not cond:
        FAILURES.append(name)


def section(title):
    print(f"\n== {title} ==")


# ---------------------------------------------------------------- 1. offline

def test_offline(scratch):
    section("Offline: golden compare, export list, doc drift")
    from PIL import Image
    a_im = Image.new("RGB", (8, 8), (10, 20, 30))
    b_im = a_im.copy()
    p1 = os.path.join(scratch, "gold_a.png")
    p2 = os.path.join(scratch, "gold_b.png")
    a_im.save(p1)
    b_im.save(p2)
    import server as _server
    rep = _server.golden_compare(p1, p2, tol=0)
    check("identical shots pass the golden compare", rep["ok"] and rep["mismatch_px"] == 0,
          str(rep))
    b_im.putpixel((5, 5), (90, 20, 30))
    b_im.save(p2)
    rep = _server.golden_compare(p1, p2, tol=4)
    check("a changed pixel fails with its coordinates and a diff mask",
          not rep["ok"] and rep["first_mismatch"] == [5, 5]
          and os.path.isfile(rep.get("diff_mask", "")), str(rep))

    # --- the frozen export list mktat checks the ELF against (pure parsing here;
    #     the live nm run is exercised by every build_package)
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import mktat
    exports = mktat.frozen_exports()
    check("the frozen list parses and carries both libc and the compiler helpers",
          "memcpy" in exports and "__divsf3" in exports, str(sorted(exports))[:120])
    check("tat_assets is allowed (the one import the loader provides itself)",
          "tat_assets" in exports)
    check("nm -u output parses to the undefined names",
          mktat.undefined_symbols("                 U memcpy\n"
                                  "00000000 T main\n"
                                  "                 U __divsf3\n") == ["memcpy", "__divsf3"])

    # --- the doc-vs-header drift check pins today's drift so new drift is caught.
    #     The doc used to run ahead of the header with a planned-only section (gfx_*,
    #     ui_row, sheet_free - the trap duckhunt fell into); the spec now describes only
    #     what exists, so the expected drift in both directions is empty.
    _server.find_root()
    drift = _server.api_doc_drift()
    check("the drift check reads both files", drift is not None)
    check("the doc and the header agree exactly",
          drift["doc_only"] == [] and drift["header_only"] == [], str(drift))


# ---------------------------------------------------------------- 2. MCP protocol

class Rpc:
    """One server.py process spoken to the way the host speaks to it."""

    def __init__(self):
        env = dict(os.environ, GYROTRON_ROOT=ROOT)
        self.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "server.py")], cwd=ROOT, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8")
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()
        self.next_id = 0

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.put(line.strip())

    def call(self, method, params=None, notify=False, timeout=60):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            self.next_id += 1
            msg["id"] = self.next_id
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        if notify:
            return None
        try:
            line = self.lines.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError(f"server did not answer {method} within {timeout}s")
        resp = json.loads(line)
        if "error" in resp:
            raise AssertionError(f"{method}: {resp['error']}")
        return resp["result"]

    def tool(self, name, args, timeout=620):
        r = self.call("tools/call", {"name": name, "arguments": args}, timeout=timeout)
        return json.loads(r["content"][0]["text"])

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.wait(timeout=10)


KEPT_TOOLS = {"get_log", "watch_control", "create_game", "list_games", "compile_game", "build_package",
              "screenshot", "validate_package", "install_to_watch"}


def test_mcp():
    section("MCP protocol (server.py over stdio)")
    # A failed earlier run must not wedge this one - clean first.
    shutil.rmtree(os.path.join(ROOT, "games", "selftest_tmp"), ignore_errors=True)
    rpc = Rpc()
    try:
        info = rpc.call("initialize", {"protocolVersion": "2025-06-18",
                                       "capabilities": {}, "clientInfo": {"name": "selftest"}})
        check("initialize answers", info["serverInfo"]["name"] == "gyro-tron-dev")
        rpc.call("notifications/initialized", notify=True)

        names = {t["name"] for t in rpc.call("tools/list")["tools"]}
        check("the tool list is exactly the device tooling", names == KEPT_TOOLS,
              str(sorted(names)))

        out = rpc.tool("create_game", {"id": "selftest_tmp", "name": "SELFTEST", "author": "tests"})
        check("scratch game created", "error" not in out, str(out)[:200])
        check("the placeholder icon fits the link-frame budget",
              not out.get("icon", {}).get("error"), str(out.get("icon"))[:200])
        c_path = os.path.join(ROOT, "games", "selftest_tmp", "selftest_tmp.c")
        check("the template source landed", os.path.isfile(c_path))
    finally:
        rpc.close()
        shutil.rmtree(os.path.join(ROOT, "games", "selftest_tmp"), ignore_errors=True)


# ---------------------------------------------------------------- main

def main():
    scratch = os.path.join(ROOT, "build", "selftest")
    os.makedirs(scratch, exist_ok=True)
    try:
        test_offline(scratch)
        test_mcp()
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("\n" + ("ALL OK" if not FAILURES else f"FAILED: {len(FAILURES)} - " + ", ".join(FAILURES)))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
