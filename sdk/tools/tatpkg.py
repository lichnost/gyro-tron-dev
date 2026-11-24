"""The .tat container and its signature, in one importable place.

Three consumers must agree about this format byte for byte - the packer
(tools/mktat.py), the validators (the dev plugin's MCP server, the catalog builder) and
the loaders (components/loader on the watch, TatPackage.cs in the manager app). Until
this module existed each carried its own parse, and a format change could ship past two
of the three and only fail on a real watch. Everything here is stdlib-only so it runs
everywhere the SDK runs; the Ed25519 arithmetic is the reference implementation shape
(RFC 8032) and is cross-checked against the `cryptography` package by tests.

Container layout: docs/reference/GAME_API.md section 2. 96-byte header, section table,
sections. The SIG section is a signature over everything before it:

    type 'SIG ', payload = 1 key-id byte + 64 Ed25519 bytes, always the LAST section;
    the table entry's own crc32 is 0 - the signature is the section's integrity check,
    and a CRC over the signature bytes would be circular (the signature covers the table
    entry that would carry the CRC).

Signed means the official channel: built and signed by the console developers. Community
packages are unsigned by design - their risk is bounded by the loader sandbox, not by a
signature. An absent SIG section on an old package is normal and installs as ever.

Command line:

    python tools/tatpkg.py verify <file.tat>...    container + signature verdict
    python tools/tatpkg.py genkey <seed-file> <pub-file>
        Generate an Ed25519 keypair, hex one per line. The seed file is SECRET: the
        official channel's seed lives only in catalog CI variables, never in a repo.
"""

import hashlib
import os
import re
import struct
import sys
import zlib

MAGIC = b"TATPKG\0\x01"
KIND_GAME = 1
HEADER_BYTES = 96
MAX_SECTIONS = 64

SEC_CODE = b"CODE"
SEC_ICON = b"ICON"
SEC_ASSET = b"ASST"
SEC_SIG = b"SIG "
ICON_MAX_PX = 232
ICON_MIN_PX = 200
ICON_MAX_BYTES = 4096
SIG_SECTION_BYTES = 1 + 64          # key id + Ed25519 signature

# Verdicts of verify_signature(); an unknown key is INVALID on purpose: a firmware too
# old for a rotated key must refuse the package rather than badge-less accept it.
SIG_ABSENT = "absent"
SIG_VALID = "valid"
SIG_INVALID = "invalid"

# The official channel's public signing keys, selected by the SIG section's key-id byte.
# The private halves live only in catalog CI variables. Rotation adds a row here, a row
# in components/loader/keys.c and one in the manager's TatPackage.cs - all three carry
# the same table.
OFFICIAL_KEYS = {
    0: bytes.fromhex("9ece320a08a2b66f0ba3f594d05cdab3d4c345688571f9d89c00953080868213"),
}


def trusted_keys():
    """The keys a signature may verify against: the official table, plus whatever
    GYROTRON_TAT_PUBKEYS adds - "id:hex" pairs, comma separated, bare hex meaning id 0.
    The env is how tests and local catalog work point the tools at their own keys."""
    keys = dict(OFFICIAL_KEYS)
    for item in os.environ.get("GYROTRON_TAT_PUBKEYS", "").split(","):
        item = item.strip()
        if not item:
            continue
        key_id, _, hexkey = item.partition(":")
        if not hexkey:
            hexkey, key_id = key_id, "0"
        keys[int(key_id)] = bytes.fromhex(hexkey)
    return keys


# ---------------------------------------------------------------- the container


def api_constants(root):
    """TAT_API_MAJOR / TAT_API_MINOR straight from the header, so nothing goes stale.
    Missing file -> (1, 0), the same fallback the dev plugin has always had."""
    out = {"major": 1, "minor": 0}
    try:
        text = open(os.path.join(root, "components", "tat_api", "include", "tat",
                                 "tat_api.h"), encoding="utf-8", errors="replace").read()
        for key, name in (("major", "TAT_API_MAJOR"), ("minor", "TAT_API_MINOR")):
            m = re.search(rf"#define {name}\s+(\d+)", text)
            if m:
                out[key] = int(m.group(1))
    except OSError:
        pass
    return out


def parse(data, api=None):
    """Full container validation, per docs/reference/GAME_API.md section 2: magic,
    format version, total size, header CRC, every section's size and CRC, kind, icon
    sanity. Returns the verdict dict the dev plugin's validator has always returned, so
    its tool stays shape-compatible, plus the SIG section's facts (verification itself
    is verify_signature - it needs keys, which are policy).

    `api` is the firmware to check against, as {"major": n, "minor": n}; None skips the
    compatibility check (the catalog builder supplies its own, the manager has no
    opinion - the connected watch does)."""
    errors, checks = [], []

    if len(data) < HEADER_BYTES or data[:7] != b"TATPKG\x00":
        return {"ok": False, "errors": ["not a .tat package (bad magic)"]}
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

    if section_count == 0 or section_count > MAX_SECTIONS:
        errors.append(f"the section table makes no sense ({section_count} sections)")
        return {"ok": False, "errors": errors, "bytes": len(data)}

    hdr_crc = zlib.crc32(data[12:HEADER_BYTES + 16 * section_count]) & 0xFFFFFFFF
    (stored_crc,) = _u32(data, 8)
    checks.append(("header crc32", hdr_crc == stored_crc,
                   f"stored {stored_crc:#010x} computed {hdr_crc:#010x}"))
    if hdr_crc != stored_crc:
        errors.append("header CRC mismatch")
    checks.append(("total_size", total_size == len(data), f"{total_size} vs {len(data)}"))
    if total_size != len(data):
        errors.append(f"total_size {total_size} does not match the file ({len(data)})")

    sections, signature = [], {"present": False}
    for i in range(section_count):
        off = HEADER_BYTES + 16 * i
        (soff,) = _u32(data, off + 4)
        (ssize,) = _u32(data, off + 8)
        (scrc,) = _u32(data, off + 12)
        label = data[off:off + 4].decode("ascii", "replace")
        body = data[soff:soff + ssize]
        if len(body) != ssize:
            errors.append(f"section {label}: runs past the end of the file")
            sections.append({"type": label, "offset": soff, "size": ssize, "crc_ok": False})
            continue
        # The SIG section's table entry carries 0 by format rule - its integrity is the
        # signature itself (see the module docstring).
        ok = scrc == 0 if label == "SIG " else (zlib.crc32(body) & 0xFFFFFFFF) == scrc
        if not ok:
            errors.append(f"section {label}: bad size or CRC")
        info = {"type": label, "offset": soff, "size": ssize, "crc_ok": ok}
        if label == "ASST" and ssize >= 16:
            info["asset"] = body[:16].split(b"\x00")[0].decode("utf-8", "replace")
        if label == "SIG ":
            if signature["present"]:
                errors.append("two SIG sections")
            signature = {"present": True, "offset": soff, "size": ssize}
            if ssize != SIG_SECTION_BYTES:
                errors.append(f"SIG section is {ssize} bytes, not {SIG_SECTION_BYTES}")
            elif soff + ssize != len(data):
                errors.append("the SIG section is not the last thing in the file")
            else:
                signature["key_id"] = body[0]
        sections.append(info)

    if api is not None:
        compatible = kind == 1 and api_maj == api["major"] and api_min <= api["minor"]
        checks.append(("api version", compatible,
                       f"package {api_maj}.{api_min}, firmware {api['major']}.{api['minor']}"))
        if not compatible:
            errors.append("api version: this watch firmware would grey the icon out")
    if kind != 1:
        # The loader refuses anything but KIND_GAME, so the validator says the same.
        errors.append(f"kind {kind}: only KIND_GAME (1) exists, values 2+ are unassigned")

    icon = {}
    for s in sections:
        if s["type"] != "ICON":
            continue
        png = data[s["offset"]:s["offset"] + s["size"]]
        w = h = None
        if png[:8] == b"\x89PNG\r\n\x1a\n" and len(png) > 24:
            w = int.from_bytes(png[16:20], "big")
            h = int.from_bytes(png[20:24], "big")
        icon = {"bytes": s["size"], "png": png[:8] == b"\x89PNG\r\n\x1a\n",
                "width": w, "height": h}
        if s["size"] > ICON_MAX_BYTES:
            errors.append(f"icon is {s['size']} bytes, limit {ICON_MAX_BYTES}")
        if w and (w > ICON_MAX_PX or h > ICON_MAX_PX):
            errors.append(f"icon is {w}x{h}, launcher decodes at most {ICON_MAX_PX}x{ICON_MAX_PX}")
        if w and (w < ICON_MIN_PX or h < ICON_MIN_PX):
            errors.append(f"icon is {w}x{h}, expected at least {ICON_MIN_PX}x{ICON_MIN_PX}")

    return {
        "ok": not errors, "errors": errors,
        "kind": "GAME" if kind == 1 else kind, "id": pid, "name": name, "author": author,
        "version": pkg_ver, "accent_rgb": list(accent), "bytes": len(data),
        "api": {"major": api_maj, "minor": api_min},
        "icon": icon, "sections": sections, "signature": signature,
        "checks": [{"check": c, "ok": ok, "detail": d} for c, ok, d in checks],
    }


def icon_png(data):
    """The package's ICON section bytes, or None. The catalog builder lifts the icon out
    of the package so the index never disagrees with what the watch will show."""
    v = parse(data)
    for s in v.get("sections", []):
        if s["type"] == "ICON":
            return data[s["offset"]:s["offset"] + s["size"]]
    return None


def _u32(b, off):
    return (int.from_bytes(b[off:off + 4], "little"),)


def _u16(b, off):
    return (int.from_bytes(b[off:off + 2], "little"),)


# ---------------------------------------------------------------- Ed25519, RFC 8032
# The classic reference shape: affine curve points and big-int arithmetic. Slow by
# modern standards (about a second to sign a large package) and fine for tools; the
# watch's own verifier is C (components/loader/sig.c) and must agree with this on every
# verdict - tests/test_sig.py holds both to the same vectors.

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = None            # filled in below; needs modular inversion first


def _inv(x):
    return pow(x, _P - 2, _P)


_D = -121665 * _inv(121666) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _xrecover(y):
    xx = (y * y - 1) * _inv(_D * y * y + 1)
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = x * _SQRT_M1 % _P
    if x % 2 != 0:
        x = _P - x
    return x


_BY = 4 * _inv(5)
_B = [_xrecover(_BY) % _P, _BY]


def _edwards_add(P, Q):
    x1, y1 = P
    x2, y2 = Q
    x3 = (x1 * y2 + x2 * y1) * _inv(1 + _D * x1 * x2 * y1 * y2)
    y3 = (y1 * y2 + x1 * x2) * _inv(1 - _D * x1 * x2 * y1 * y2)
    return [x3 % _P, y3 % _P]


def _scalarmult(P, e):
    if e == 0:
        return [0, 1]
    Q = _scalarmult(P, e // 2)
    Q = _edwards_add(Q, Q)
    if e & 1:
        Q = _edwards_add(Q, P)
    return Q


def _on_curve(P):
    x, y = P
    return (-x * x + y * y - 1 - _D * x * x * y * y) % _P == 0


def _encode_point(P):
    x, y = P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decode_point(s):
    if len(s) != 32:
        raise ValueError("point encoding must be 32 bytes")
    y = int.from_bytes(s, "little")
    if y >> 255 not in (0, 1) or (y & ~(1 << 255)) >= _P:
        raise ValueError("non-canonical point encoding")
    x = _xrecover(y & ~(1 << 255))
    if x & 1 != (y >> 255) & 1:
        x = _P - x
    P = [x, y & ~(1 << 255)]
    if not _on_curve(P):
        raise ValueError("point is not on the curve")
    return P


def _clamp(h32):
    return 2**254 + (int.from_bytes(h32, "little") & (2**254 - 8))


def public_key(seed):
    """The 32-byte public half of a 32-byte seed."""
    if len(seed) != 32:
        raise ValueError("the seed must be 32 bytes")
    return _encode_point(_scalarmult(_B, _clamp(hashlib.sha512(seed).digest()[:32])))


def sign(message, seed):
    """Deterministic RFC 8032 signature (64 bytes) over the message."""
    if len(seed) != 32:
        raise ValueError("the seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = _clamp(h[:32])
    pub = _encode_point(_scalarmult(_B, a))
    r = int.from_bytes(hashlib.sha512(h[32:] + message).digest(), "little")
    R = _encode_point(_scalarmult(_B, r))
    k = int.from_bytes(hashlib.sha512(R + pub + message).digest(), "little") % _L
    s = (r + k * a) % _L
    return R + s.to_bytes(32, "little")


def verify(signature64, message, pk):
    """True when the 64-byte signature verifies over the message with the 32-byte
    public key. The non-cofactored equation of RFC 8032, with the canonicality checks
    (S below the group order, decodable points) that make malleable forgeries fail."""
    if len(signature64) != 64 or len(pk) != 32:
        return False
    try:
        R = _decode_point(signature64[:32])
        A = _decode_point(pk)
    except ValueError:
        return False
    S = int.from_bytes(signature64[32:], "little")
    if S >= _L:
        return False
    h = int.from_bytes(hashlib.sha512(signature64[:32] + pk + message).digest(), "little")
    return _scalarmult(_B, S) == _edwards_add(R, _scalarmult(A, h))


# ---------------------------------------------------------------- the SIG section


def sign_package(data, seed, key_id=0):
    """Append the SIG section to a package and return the new bytes. The signature
    covers everything from offset 0 to the start of the section itself - header, table,
    every other section - so nothing about the package can change after signing. The
    header grows by one table entry, which moves nothing (offsets are absolute) but
    does change the header CRC; the whole prefix is rebuilt here for that reason."""
    v = parse(data)
    if not v["ok"]:
        raise ValueError("not a sound package: " + "; ".join(v["errors"]))
    if v["signature"]["present"]:
        raise ValueError("the package is already signed")

    sections = [(s["type"].encode(), data[s["offset"]:s["offset"] + s["size"]])
                for s in v["sections"]]

    table, blobs, off = b"", b"", HEADER_BYTES + 16 * (len(sections) + 1)
    for tag, payload in sections:
        table += struct.pack("<4sIII", tag, off, len(payload),
                             zlib.crc32(payload) & 0xFFFFFFFF)
        blobs += payload
        off += len(payload)
    table += struct.pack("<4sIII", SEC_SIG, off, SIG_SECTION_BYTES, 0)

    # Bytes 16..96 of the old header survive verbatim except the section count.
    body = data[16:92] + struct.pack("<H", len(sections) + 1) + b"\0\0"
    total = off + SIG_SECTION_BYTES
    after = struct.pack("<I", total) + body + table
    prefix = MAGIC[:7] + b"\x01" + struct.pack("<I", zlib.crc32(after) & 0xFFFFFFFF) \
        + after + blobs
    assert len(prefix) == off
    return prefix + bytes([key_id]) + sign(prefix, seed)


def verify_signature(data, keys=None):
    """(verdict, detail) for a package: SIG_ABSENT (nothing signed - old or community),
    SIG_VALID, or SIG_INVALID with the reason. Any container damage is INVALID, not
    ABSENT: a damaged file must not fall back to looking unsigned."""
    keys = trusted_keys() if keys is None else keys
    v = parse(data)
    if not v["ok"]:
        return SIG_INVALID, "the package is not sound: " + "; ".join(v["errors"])
    sig = v["signature"]
    if not sig["present"]:
        return SIG_ABSENT, "no SIG section"
    if "key_id" not in sig:
        return SIG_INVALID, "the SIG section is malformed"
    if sig["key_id"] not in keys:
        return SIG_INVALID, f"signed with key id {sig['key_id']}, which this build does not know"
    body = data[sig["offset"]:sig["offset"] + sig["size"]]
    if verify(body[1:], data[:sig["offset"]], keys[sig["key_id"]]):
        return SIG_VALID, f"verified against key {sig['key_id']}"
    return SIG_INVALID, "the signature does not verify"


# ---------------------------------------------------------------- command line


def read_seed(path):
    text = open(path).read().strip()
    if len(text) == 128:            # seed||public dump; the seed is the first half
        text = text[:64]
    if len(text) != 64:
        sys.exit(f"{path} is not a 32-byte hex seed")
    return bytes.fromhex(text)


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what == "verify" and len(sys.argv) > 2:
        fail = 0
        for path in sys.argv[2:]:
            data = open(path, "rb").read()
            v = parse(data)
            state, detail = verify_signature(data)
            print(f"{path}: {'sound' if v['ok'] else 'BROKEN'} package"
                  f" {v.get('id', '?')} v{v.get('version', '?')}"
                  f" by {v.get('author', '?')}; signature {state} ({detail})")
            fail += 0 if v["ok"] and state != SIG_INVALID else 1
        return fail
    if what == "genkey" and len(sys.argv) == 4:
        seed = os.urandom(32)
        open(sys.argv[2], "w").write(seed.hex() + "\n")
        open(sys.argv[3], "w").write(public_key(seed).hex() + "\n")
        print(f"seed -> {sys.argv[2]} (SECRET - goes to CI variables, never a repo)")
        print(f"public key -> {sys.argv[3]}")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
