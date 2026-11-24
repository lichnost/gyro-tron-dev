"""Collect community catalog entries from the registry into a staging tree.

    python tools/collect_registry.py <registry-dir> <staging-out> [--mktat PATH] [--work DIR]

The registry directory holds one JSON file per game (this is `gyro-tron-games`'
`registry/`; the same file shape its PR review approves):

    {
      "id": "bunnyhop",
      "repo": "https://github.com/someone/bunnyhop",
      "commit": "0123abcd...",           // pinned SHA, never a branch
      "summary": "Hop the bunny over fences",
      "dir": "game",                     // optional: the game folder in the repo
                                         // (default: the repo root)
      "package": "dist/bunnyhop.tat"     // optional: fetch this pre-built package
                                         // instead of packing the sources
    }

Two ways into the community channel, per docs/reference/CATALOG.md:

- **From source** (no `package` key): the folder at `dir` is a game folder - game.json
  at its root - and the packer builds it (`tools/mktat.py`). Every shipped package is
  reproducible from the pinned commit.
- **Pre-built** (`package` key): the `.tat` is taken from the author's repository at
  the pinned commit, verbatim. The agreement is: the file lives inside the reviewed
  repo, the pinned commit fixes its bytes exactly (git history is the integrity
  guarantee), and the author may not serve the package from anywhere else. Updates
  ride the same review as everything else - a commit bump re-fetches and re-checks.

Rules both ways: the package's id must equal the entry's id; the community channel
stays unsigned (a SIG section is refused here, and again by make_catalog.py); GB
ROM-cartridge descriptors never enter; the repository carries a LICENSES.md declaring
asset provenance. `package` and `dir` are mutually exclusive and both must be
repo-relative paths without "..".

The output is `staging-out/community/<id>/` - the `.tat`, `summary.txt`, any
`shots-*.png` found beside the game - ready for `tools/make_catalog.py`, which runs
the full validation on top of this. Collected packages are also checked here so a
mismatched id or a smuggled signature is named at the collecting step, not buried in
the catalog build.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

sys.path.insert(0, HERE)
import tatpkg  # noqa: E402  the shared container parse module

ID_RE = re.compile(r"^[a-z0-9_]{1,15}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{7,40}$")
MAX_PACKAGE_BYTES = 4 * 1024 * 1024
ENTRY_KEYS = {"id", "repo", "commit", "summary", "dir", "package"}


def fail(msg):
    print(f"collect_registry: FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


def rel_path_clean(value, what, entry_id):
    """A repo-relative path a registry entry may name: forward slashes, no escaping
    the clone, no absolute paths. This is review-facing schema, so strictness here is
    the point."""
    if not value or value.startswith(("/", "\\")) or (len(value) > 1 and value[1] == ":"):
        fail(f"{entry_id}: {what} must be a repo-relative path, got {value!r}")
    if ".." in value.split("/") or "\\" in value:
        fail(f"{entry_id}: {what} must use forward slashes and never climb out (..), got {value!r}")
    return value


def load_entry(path):
    """One registry file, schema-checked. Unknown keys are refused: the schema is the
    agreement, and a typo'd key must not silently disable a rule."""
    entry_id = os.path.splitext(os.path.basename(path))[0]
    try:
        entry = json.load(open(path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        fail(f"{path}: does not parse: {e}")
    if not isinstance(entry, dict):
        fail(f"{path}: must be an object")
    unknown = set(entry) - ENTRY_KEYS
    if unknown:
        fail(f"{path}: keys the registry schema does not know: {', '.join(sorted(unknown))}")
    for key in ("repo", "commit", "summary"):
        if not isinstance(entry.get(key), str) or not entry[key].strip():
            fail(f"{path}: missing or empty {key!r}")
    if entry.get("id", entry_id) != entry_id:
        fail(f"{path}: its id field ({entry.get('id')!r}) disagrees with the file name ({entry_id})")
    if not ID_RE.fullmatch(entry_id):
        fail(f"{path}: id {entry_id!r} is not lowercase [a-z0-9_], max 15 chars")
    if not COMMIT_RE.fullmatch(entry["commit"]):
        fail(f"{path}: commit must be a hex SHA (7..40), got {entry['commit']!r} - never a branch name")
    if len(entry["summary"].strip().splitlines()[0].strip()) > 64 or not entry["summary"].strip():
        fail(f"{path}: summary must be one line of 1..64 chars")
    package = entry.get("package")
    folder = entry.get("dir")
    if package is not None and folder is not None:
        fail(f"{path}: 'package' (fetch a pre-built .tat) and 'dir' (pack these sources) "
             "are mutually exclusive")
    if package is not None:
        package = rel_path_clean(package, "package", entry_id)
        if not package.endswith(".tat"):
            fail(f"{path}: package must name a .tat, got {package!r}")
        entry["package"] = package
    elif folder is not None:
        entry["dir"] = rel_path_clean(folder, "dir", entry_id)
    return entry_id, entry


def rmtree_ro(path):
    """rmtree that clears the read-only bit git puts on its object files - a plain
    rmtree dies on Windows the moment a .git directory is involved. A path that does
    not exist yet (a fresh work dir) is already as clean as it needs to be."""
    if not os.path.exists(path):
        return
    def relax(func, p, _exc):
        os.chmod(p, 0o666)
        func(p)
    shutil.rmtree(path, onerror=relax)


def clone_at(repo, commit, work_dir, entry_id):
    """The author's repository at exactly the pinned commit. The clone is the only
    thing this tool reads a package from; a commit the repo does not have is an error,
    not a silent checkout of whatever is newest."""
    dest = os.path.join(work_dir, entry_id, "clone")
    if os.path.exists(dest):
        rmtree_ro(dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    r = subprocess.run(["git", "clone", "--quiet", repo, dest], capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{entry_id}: cannot clone {repo}: {r.stderr.strip()}")
    r = subprocess.run(["git", "-C", dest, "-c", "advice.detachedHead=false",
                        "checkout", "--quiet", "--detach", commit], capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{entry_id}: {repo} has no commit {commit}: {r.stderr.strip()}")
    if not os.path.exists(os.path.join(dest, "LICENSES.md")):
        fail(f"{entry_id}: {repo} carries no LICENSES.md declaring asset provenance - "
             "the registry refuses entries without one")
    return dest


def check_package(data, entry_id):
    """The checks that are the registry's own (the catalog build repeats the rest):
    sound container, the id the entry claims, unsigned."""
    if len(data) > MAX_PACKAGE_BYTES:
        fail(f"{entry_id}: the package is {len(data)} bytes, the cap is {MAX_PACKAGE_BYTES}")
    v = tatpkg.parse(data)
    if not v["ok"]:
        fail(f"{entry_id}: the package does not validate: {'; '.join(v['errors'])}")
    if v["id"] != entry_id:
        fail(f"{entry_id}: the package says its id is {v['id']!r}")
    if v["signature"]["present"]:
        fail(f"{entry_id}: the package carries a SIG section - community packages are "
             "never signed (docs/reference/CATALOG.md)")


def write_entry(staging, entry_id, data, summary, shots):
    out_dir = os.path.join(staging, "community", entry_id)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, entry_id + ".tat"), "wb") as f:
        f.write(data)
    with open(os.path.join(out_dir, "summary.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(summary.strip() + "\n")
    for shot in shots:
        shutil.copyfile(shot, os.path.join(out_dir, os.path.basename(shot)))
    return out_dir


def collect(entry_id, entry, staging, work_dir, mktat):
    clone = clone_at(entry["repo"], entry["commit"], work_dir, entry_id)
    summary = entry["summary"]

    if "package" in entry:
        # Pre-built: the reviewed bytes, verbatim from the pinned commit.
        src = os.path.join(clone, entry["package"])
        if not os.path.isfile(src):
            fail(f"{entry_id}: {entry['repo']} has no {entry['package']} at {entry['commit']}")
        data = open(src, "rb").read()
        check_package(data, entry_id)
        shots = sorted(glob.glob(os.path.join(os.path.dirname(src), "shots-*.png")))
        where = write_entry(staging, entry_id, data, summary, shots)
        print(f"  {entry_id:<16} pre-built  {entry['commit'][:12]}  {len(data) / 1024:7.1f} KB  -> {where}")
        return

    # From source: the folder is a game folder; the packer builds it. GB descriptors
    # are refused here, before the packer ever sees them.
    game_dir = os.path.join(clone, entry.get("dir", "."))
    meta_path = os.path.join(game_dir, "game.json")
    if not os.path.isfile(meta_path):
        fail(f"{entry_id}: no game.json in {entry.get('dir', '.')}/ of {entry['repo']} at {entry['commit']}")
    try:
        meta = json.load(open(meta_path, encoding="utf-8"))
    except ValueError as e:
        fail(f"{entry_id}: game.json does not parse: {e}")
    if meta.get("type") == "gb":
        fail(f"{entry_id}: GB ROM-cartridge descriptors never enter the community channel")
    if meta.get("id") != entry_id:
        fail(f"{entry_id}: game.json says its id is {meta.get('id')!r}")

    out_tat = os.path.join(staging, "community", entry_id, entry_id + ".tat")
    os.makedirs(os.path.dirname(out_tat), exist_ok=True)
    r = subprocess.run([sys.executable, mktat, game_dir, "--out", out_tat],
                       capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{entry_id}: the packer refused it:\n{r.stdout}{r.stderr}")
    data = open(out_tat, "rb").read()
    check_package(data, entry_id)   # the packer built it, but the id rule is the registry's
    shots = sorted(glob.glob(os.path.join(game_dir, "shots-*.png")))
    write_entry(staging, entry_id, data, summary, shots)
    print(f"  {entry_id:<16} packed     {entry['commit'][:12]}  {len(data) / 1024:7.1f} KB  "
          f"-> {os.path.dirname(out_tat)}")


def main():
    ap = argparse.ArgumentParser(description="collect registry entries into a community staging tree")
    ap.add_argument("registry", help="the registry directory (one .json per game)")
    ap.add_argument("staging", help="the staging tree to fill (its community/ is rebuilt)")
    ap.add_argument("--mktat", default=os.path.join(HERE, "mktat.py"),
                    help="the packer to build source entries with (default: this repo's)")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "registry-collect"),
                    help="where clones go (default build/registry-collect)")
    args = ap.parse_args()

    if not os.path.isdir(args.registry):
        fail(f"{args.registry} is not a directory")
    entries = []
    for name in sorted(os.listdir(args.registry)):
        if name.endswith(".json"):
            entries.append(load_entry(os.path.join(args.registry, name)))

    shutil.rmtree(os.path.join(args.staging, "community"), ignore_errors=True)
    os.makedirs(args.staging, exist_ok=True)
    rmtree_ro(args.work)

    for entry_id, entry in entries:
        collect(entry_id, entry, args.staging, args.work, args.mktat)
    print(f"{len(entries)} entries -> {os.path.join(args.staging, 'community')}")


if __name__ == "__main__":
    main()
