"""Build the game catalog: walk a staging tree, validate every package, sign the
official ones, emit the deploy tree and the two index variants.

    python tools/make_catalog.py <staging> [--out build/catalog-out] [--sign-key FILE]
                                [--api 1.9] [--generated "2026-11-24T12:00:00Z"]

The staging tree is git-reviewed state, one folder per game:

    <staging>/official/<id>/   <id>.tat, summary.txt (one line), meta.json (optional:
                               {"source": url, "min_fw": build-id}), shots-*.png
    <staging>/community/<id>/  the same shape, always unsigned
    <staging>/revoked.txt      optional: one revoked package sha256 per line

Out comes what rsync puts on the host (docs/reference/CATALOG.md has the layout and
the index schemas; index urls are relative to the /otadb/ root):

    <out>/games/index.json  index-watch.json  official/<id>.tat  community/<id>.tat
    <out>/games/img/<id>-icon.png  img/<id>-<n>.png

The signing key comes from --sign-key or TAT_SIGNING_KEY (a file path or 64 hex
chars) and is for the official channel only: the build refuses rather than sign a
community entry, and a community entry that arrives pre-signed is an error. Any
invalid entry fails the whole build - a half-valid catalog must never deploy.

Entry fields come from the parsed package header (name, author, version, api), so the
index cannot disagree with the .tat beside it; size and sha256 are computed after
signing, on the exact bytes that will be served.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

sys.path.insert(0, HERE)
import tatpkg  # noqa: E402  the shared container parse/sign module

CHANNELS = ("official", "community")
MAX_GAMES = 200            # the watch variant's hard cap; past it, the format changes on purpose
MAX_PACKAGE_BYTES = 4 * 1024 * 1024
ID_RE = re.compile(r"^[a-z0-9_]{1,15}$")
SUMMARY_MAX = 64


def fail(msg):
    print(f"make_catalog: FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


def load_signing_key(arg):
    """The Ed25519 seed from --sign-key / TAT_SIGNING_KEY: a file of hex (path) or hex
    itself. None when neither is set - the build then demands pre-signed officials."""
    val = arg or os.environ.get("TAT_SIGNING_KEY", "")
    val = val.strip()
    if not val:
        return None
    if os.path.isfile(val):
        return tatpkg.read_seed(val)
    if re.fullmatch(r"[0-9a-fA-F]{64}", val):
        return bytes.fromhex(val)
    fail(f"TAT_SIGNING_KEY is neither a file nor 64 hex chars: {val[:20]}...")


def read_meta(game_dir):
    """meta.json is optional and only carries what the package cannot: where its source
    lives and the oldest firmware build that can run it."""
    path = os.path.join(game_dir, "meta.json")
    if not os.path.exists(path):
        return {}
    try:
        meta = json.load(open(path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        fail(f"{game_dir}: meta.json does not parse: {e}")
    if not isinstance(meta, dict):
        fail(f"{game_dir}: meta.json must be an object")
    for key in meta:
        if key not in ("source", "min_fw"):
            fail(f"{game_dir}: meta.json has a key the catalog does not know: {key}")
    if not isinstance(meta.get("source", ""), str) or not isinstance(meta.get("min_fw", ""), (str, type(None))):
        fail(f"{game_dir}: meta.json source/min_fw must be strings")
    return meta


def collect(staging, channel, api):
    """Every entry of one channel, validated, signed where the channel calls for it."""
    ch_dir = os.path.join(staging, channel)
    if not os.path.isdir(ch_dir):
        return []
    found = []
    for name in sorted(os.listdir(ch_dir)):
        game_dir = os.path.join(ch_dir, name)
        if not os.path.isdir(game_dir):
            continue
        tats = [f for f in os.listdir(game_dir) if f.endswith(".tat")]
        if len(tats) != 1:
            fail(f"{game_dir}: expected exactly one .tat, found {len(tats)}")
        data = open(os.path.join(game_dir, tats[0]), "rb").read()
        if len(data) > MAX_PACKAGE_BYTES:
            fail(f"{game_dir}: {len(data)} bytes, the cap is {MAX_PACKAGE_BYTES}")

        v = tatpkg.parse(data, api=api)
        if not v["ok"]:
            fail(f"{game_dir}: {tats[0]} does not validate: {'; '.join(v['errors'])}")
        if v["id"] != name:
            fail(f"{game_dir}: the folder says {name} but the package says {v['id']}")
        if not ID_RE.fullmatch(v["id"]):
            fail(f"{game_dir}: id {v['id']!r} is not lowercase [a-z0-9_], max 15 chars")

        summary_path = os.path.join(game_dir, "summary.txt")
        if not os.path.exists(summary_path):
            fail(f"{game_dir}: no summary.txt")
        summary = open(summary_path, encoding="utf-8").read().strip().splitlines()
        summary = summary[0].strip() if summary else ""
        if not summary or len(summary) > SUMMARY_MAX:
            fail(f"{game_dir}: summary.txt needs one line of 1..{SUMMARY_MAX} chars")

        meta = read_meta(game_dir)
        shots = sorted(f for f in os.listdir(game_dir) if re.fullmatch(r"shots-\d+\.png", f))
        for shot in shots:
            if open(os.path.join(game_dir, shot), "rb").read(4) != b"\x89PNG":
                fail(f"{game_dir}: {shot} is not a PNG")

        found.append({"dir": game_dir, "data": data, "header": v, "summary": summary,
                      "meta": meta, "shots": shots})
    return found


def sign_or_check(entry, channel, seed):
    """The channel decides what a signature means here. Official: signed, by us now or
    by whoever built the .tat. Community: never - its trust boundary is the sandbox."""
    data, where = entry["data"], entry["dir"]
    state, detail = tatpkg.verify_signature(data, keys=tatpkg.trusted_keys())
    if channel == "community":
        if state != tatpkg.SIG_ABSENT:
            fail(f"{where}: a community package must not carry a signature ({detail})")
        return data
    # official
    if state == tatpkg.SIG_VALID:
        return data                        # already signed and it verifies
    if seed is not None:
        if state == tatpkg.SIG_INVALID and entry_signed(data):
            fail(f"{where}: pre-signed but the signature does not verify ({detail})")
        return tatpkg.sign_package(data, seed)
    fail(f"{where}: an official entry needs TAT_SIGNING_KEY, or a valid signature already "
         f"in the package ({detail})")


def entry_signed(data):
    return tatpkg.parse(data)["signature"]["present"]


def build_index(entries, revoked, generated):
    """The full index (manager, site) and the watch variant (§5 of the distribution
    plan): same entries, minified keys, no field the store never renders."""
    games = []
    for e in sorted(entries, key=lambda e: e["header"]["id"]):
        h = e["header"]
        games.append({
            "id": h["id"], "name": h["name"], "author": h["author"],
            "version": h["version"], "api_major": h["api"]["major"], "api_minor": h["api"]["minor"],
            "channel": e["channel"], "size": len(e["final"]), "sha256": e["sha256"],
            "url": f"games/{e['channel']}/{h['id']}.tat",
            "icon": f"games/img/{h['id']}-icon.png",
            "shots": [f"games/img/{h['id']}-{n}.png" for n in range(1, len(e["shots"]) + 1)],
            "source": e["meta"].get("source"),
            "min_fw": e["meta"].get("min_fw"),
            "summary": e["summary"],
        })
    return {"schema": 1, "generated": generated, "games": games, "revoked": revoked}


def build_watch_index(index):
    """The compact variant: id, name, author, version, api, channel, size, sha256, url,
    icon, summary - one-char keys, channel as one char. A watch parses this into a
    fixed-size array; the cap below keeps that array bounded by construction."""
    games = []
    for g in index["games"]:
        games.append({
            "i": g["id"], "n": g["name"], "a": g["author"], "v": g["version"],
            "p": f"{g['api_major']}.{g['api_minor']}", "c": g["channel"][0],
            "z": g["size"], "h": g["sha256"], "u": g["url"], "ic": g["icon"],
            "s": g["summary"],
        })
    return {"schema": 1, "generated": index["generated"], "games": games,
            "revoked": index["revoked"]}


def main():
    ap = argparse.ArgumentParser(description="build the game catalog from a staging tree")
    ap.add_argument("staging", help="the reviewed tree of official/<id>/ and community/<id>/ folders")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "catalog-out"),
                    help="where the deploy tree goes (default build/catalog-out)")
    ap.add_argument("--sign-key", help="the official channel's Ed25519 seed (file path); "
                                       "TAT_SIGNING_KEY is the env equivalent")
    ap.add_argument("--api", help='check packages against this api, e.g. "1.9" (default: '
                                  "the header this repo/SDK carries)")
    ap.add_argument("--generated", help="override the index `generated` stamp (deterministic rebuilds)")
    args = ap.parse_args()

    staging = os.path.abspath(args.staging)
    if not os.path.isdir(staging):
        fail(f"{staging} is not a directory")
    for name in os.listdir(staging):
        if os.path.isdir(os.path.join(staging, name)) and name not in CHANNELS:
            fail(f"{staging}: {name}/ is not a channel (the channels are {CHANNELS})")

    if args.api:
        m = re.fullmatch(r"(\d+)\.(\d+)", args.api)
        if not m:
            fail('--api wants "major.minor"')
        api = {"major": int(m.group(1)), "minor": int(m.group(2))}
    else:
        api = tatpkg.api_constants(ROOT)

    seed = load_signing_key(args.sign_key)
    generated = args.generated or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    revoked = []
    revoked_path = os.path.join(staging, "revoked.txt")
    if os.path.exists(revoked_path):
        for ln, line in enumerate(open(revoked_path, encoding="utf-8"), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if not re.fullmatch(r"[0-9a-f]{64}", line):
                fail(f"{revoked_path}:{ln}: not a sha256")
            revoked.append(line)

    entries = []
    seen = {}
    for channel in CHANNELS:
        for e in collect(staging, channel, api):
            e["channel"] = channel
            e["final"] = sign_or_check(e, channel, seed)
            e["sha256"] = hashlib.sha256(e["final"]).hexdigest()
            if e["sha256"] in revoked:
                fail(f"{e['dir']}: this exact package is on the revoked list - remove one or the other")
            if e["header"]["id"] in seen:
                fail(f"{e['dir']}: id {e['header']['id']!r} is already taken by {seen[e['header']['id']]}"
                     " (catalog names are global; official wins only by the other going away)")
            seen[e["header"]["id"]] = e["dir"]
            entries.append(e)

    index = build_index(entries, revoked, generated)
    watch = build_watch_index(index)
    if len(watch["games"]) > MAX_GAMES:
        fail(f"{len(watch['games'])} games: the watch index is capped at {MAX_GAMES} entries")

    games_dir = os.path.join(args.out, "games")
    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(os.path.join(games_dir, "img"), exist_ok=True)

    for e in entries:
        hid = e["header"]["id"]
        os.makedirs(os.path.join(games_dir, e["channel"]), exist_ok=True)
        with open(os.path.join(games_dir, e["channel"], hid + ".tat"), "wb") as f:
            f.write(e["final"])
        icon = tatpkg.icon_png(e["final"])
        if icon is None:
            fail(f"{e['dir']}: the package carries no icon to lift into the index")
        with open(os.path.join(games_dir, "img", hid + "-icon.png"), "wb") as f:
            f.write(icon)
        for n, shot in enumerate(e["shots"], 1):
            shutil.copyfile(os.path.join(e["dir"], shot),
                            os.path.join(games_dir, "img", f"{hid}-{n}.png"))

    for name, doc in (("index.json", index), ("index-watch.json", watch)):
        with open(os.path.join(games_dir, name), "w", encoding="utf-8", newline="\n") as f:
            if name == "index.json":
                json.dump(doc, f, sort_keys=True, indent=2, ensure_ascii=False)
                f.write("\n")
            else:
                json.dump(doc, f, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
                f.write("\n")

    for e in sorted(entries, key=lambda e: e["header"]["id"]):
        h = e["header"]
        print(f"  {h['id']:<16} {e['channel']:<9} v{h['version']:<4} api {h['api']['major']}.{h['api']['minor']}"
              f"  {len(e['final']) / 1024:7.1f} KB  {h['name']}")
    watch_bytes = os.path.getsize(os.path.join(games_dir, "index-watch.json"))
    print(f"{len(entries)} games, {len(revoked)} revoked; watch index {watch_bytes} bytes"
          f" (cap {MAX_GAMES} entries)")
    if seed is None and any(e["channel"] == "official" for e in entries):
        print("note: no signing key given - official entries carried their own signatures")


if __name__ == "__main__":
    main()
