// The console's trusted package-signing keys. The private halves live only in catalog
// CI variables; this table (and its twins in tools/tatpkg.py and the manager's
// TatPackage.cs) is the whole public side. Rotation adds a row with a fresh key id -
// old rows stay, so firmware updated past a rotation still accepts everything signed
// before it.
#include "loader/sig.h"

const loader_signing_key_t loader_signing_keys[] = {
    { 0, {
        0x9e, 0xce, 0x32, 0x0a, 0x08, 0xa2, 0xb6, 0x6f, 0x0b, 0xa3, 0xf5, 0x94, 0xd0, 0x5c, 0xda, 0xb3,
        0xd4, 0xc3, 0x45, 0x68, 0x85, 0x71, 0xf9, 0xd8, 0x9c, 0x00, 0x95, 0x30, 0x80, 0x86, 0x82, 0x13,
    } },
};

const size_t loader_signing_key_count = sizeof loader_signing_keys / sizeof loader_signing_keys[0];
