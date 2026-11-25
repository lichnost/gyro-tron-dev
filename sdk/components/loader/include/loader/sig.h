// Package signatures: the SIG section and how the console checks it.
//
// A .tat may end with a SIG section (docs/reference/GAME_API.md section 2): 1 key-id
// byte plus a 64-byte Ed25519 signature over everything before it. Signed means the
// official channel; community packages ship unsigned on purpose - their risk is bounded
// by the loader sandbox, the signature's job is proving the provenance of official
// releases. Verification runs at install time (whoever installs calls it), never at
// launch: boot stays as fast as it is today, and the section is skipped by the code
// paths that load a game.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The verdict on a package's signature. ABSENT is not a failure: packages packed
// before signatures existed, and every community package, install exactly as they
// always did. INVALID covers a broken signature and a signature made with a key this
// build does not know - a firmware too old for a rotated key refuses the package
// rather than accepting it badge-less.
typedef enum {
    LOADER_SIG_ABSENT = 0,
    LOADER_SIG_VALID = 1,
    LOADER_SIG_INVALID = 2,
} loader_sig_t;

// One trusted public key, selected by the key-id byte inside the SIG section. Rotation
// is a new key with a new id; old and new stay compiled in together. Private halves
// live only in catalog CI variables - the same table exists in backend/tools/tatpkg.py and the
// manager's TatPackage.cs, and the three must not drift.
typedef struct {
    uint8_t key_id;
    uint8_t public_key[32];
} loader_signing_key_t;

extern const loader_signing_key_t loader_signing_keys[];
extern const size_t loader_signing_key_count;

// The verdict for a package held in memory (tests, loader_inspect callers).
loader_sig_t loader_verify_signature(const uint8_t *buf, size_t len);

// The same verdict for a package on storage, read in chunks: a 4 MB package is never
// held whole for this. Two passes over the file, the same read pattern the loader's
// own CRC check makes. NULL when the file cannot be read at all (INVALID's caller
// already has an errno story; ABSENT/VALID/INVALID is about the bytes, not the disk).
loader_sig_t loader_verify_signature_file(const char *path);

#ifdef __cplusplus
}
#endif
