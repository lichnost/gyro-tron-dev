// Ed25519 verification of a package's SIG section, self-contained on purpose.
//
// ESP-IDF's mbedtls (3.x) has no Ed25519, and the verifier must also compile for the
// PC: the emulator's loader probe and the host tests build it unchanged, so it pulls
// in nothing beyond the C library. The arithmetic is the widely-reviewed TweetNaCl
// shape (16 signed 64-bit limbs, radix 2^16); the point equation is the non-cofactored
// check of RFC 8032, s*B == R + h*A, which is what the Python reference in
// backend/tools/tatpkg.py checks too - tests/test_sig.c holds both to the same vectors.
//
// Speed is not a goal here: this runs once per install, and a few dozen milliseconds
// of double-and-add on a 512-bit scalar is nothing next to the download that preceded
// it. Launches never call this (see sig.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader/sig.h"

#define SIG_HEADER_BYTES 96
#define SIG_TABLE_ENTRY 16
#define SIG_MAX_SECTIONS 64
#define SIG_SECTION_BYTES 65        /* key id + Ed25519 signature */
#define SIG_FILE_CHUNK 2048

// ---------------------------------------------------------------- SHA-512 (FIPS 180-4)

typedef struct {
    uint64_t h[8];
    uint64_t bytes;                 /* message length so far; 2^61 bytes is beyond generous */
    uint8_t buf[128];
    size_t fill;
} sha512_t;

static const uint64_t sha512_iv[8] = {
    UINT64_C(0x6A09E667F3BCC908), UINT64_C(0xBB67AE8584CAA73B),
    UINT64_C(0x3C6EF372FE94F82B), UINT64_C(0xA54FF53A5F1D36F1),
    UINT64_C(0x510E527FADE682D1), UINT64_C(0x9B05688C2B3E6C1F),
    UINT64_C(0x1F83D9ABFB41BD6B), UINT64_C(0x5BE0CD19137E2179),
};

static const uint64_t sha512_k[80] = {
    UINT64_C(0x428A2F98D728AE22), UINT64_C(0x7137449123EF65CD), UINT64_C(0xB5C0FBCFEC4D3B2F), UINT64_C(0xE9B5DBA58189DBBC),
    UINT64_C(0x3956C25BF348B538), UINT64_C(0x59F111F1B605D019), UINT64_C(0x923F82A4AF194F9B), UINT64_C(0xAB1C5ED5DA6D8118),
    UINT64_C(0xD807AA98A3030242), UINT64_C(0x12835B0145706FBE), UINT64_C(0x243185BE4EE4B28C), UINT64_C(0x550C7DC3D5FFB4E2),
    UINT64_C(0x72BE5D74F27B896F), UINT64_C(0x80DEB1FE3B1696B1), UINT64_C(0x9BDC06A725C71235), UINT64_C(0xC19BF174CF692694),
    UINT64_C(0xE49B69C19EF14AD2), UINT64_C(0xEFBE4786384F25E3), UINT64_C(0x0FC19DC68B8CD5B5), UINT64_C(0x240CA1CC77AC9C65),
    UINT64_C(0x2DE92C6F592B0275), UINT64_C(0x4A7484AA6EA6E483), UINT64_C(0x5CB0A9DCBD41FBD4), UINT64_C(0x76F988DA831153B5),
    UINT64_C(0x983E5152EE66DFAB), UINT64_C(0xA831C66D2DB43210), UINT64_C(0xB00327C898FB213F), UINT64_C(0xBF597FC7BEEF0EE4),
    UINT64_C(0xC6E00BF33DA88FC2), UINT64_C(0xD5A79147930AA725), UINT64_C(0x06CA6351E003826F), UINT64_C(0x142929670A0E6E70),
    UINT64_C(0x27B70A8546D22FFC), UINT64_C(0x2E1B21385C26C926), UINT64_C(0x4D2C6DFC5AC42AED), UINT64_C(0x53380D139D95B3DF),
    UINT64_C(0x650A73548BAF63DE), UINT64_C(0x766A0ABB3C77B2A8), UINT64_C(0x81C2C92E47EDAEE6), UINT64_C(0x92722C851482353B),
    UINT64_C(0xA2BFE8A14CF10364), UINT64_C(0xA81A664BBC423001), UINT64_C(0xC24B8B70D0F89791), UINT64_C(0xC76C51A30654BE30),
    UINT64_C(0xD192E819D6EF5218), UINT64_C(0xD69906245565A910), UINT64_C(0xF40E35855771202A), UINT64_C(0x106AA07032BBD1B8),
    UINT64_C(0x19A4C116B8D2D0C8), UINT64_C(0x1E376C085141AB53), UINT64_C(0x2748774CDF8EEB99), UINT64_C(0x34B0BCB5E19B48A8),
    UINT64_C(0x391C0CB3C5C95A63), UINT64_C(0x4ED8AA4AE3418ACB), UINT64_C(0x5B9CCA4F7763E373), UINT64_C(0x682E6FF3D6B2B8A3),
    UINT64_C(0x748F82EE5DEFB2FC), UINT64_C(0x78A5636F43172F60), UINT64_C(0x84C87814A1F0AB72), UINT64_C(0x8CC702081A6439EC),
    UINT64_C(0x90BEFFFA23631E28), UINT64_C(0xA4506CEBDE82BDE9), UINT64_C(0xBEF9A3F7B2C67915), UINT64_C(0xC67178F2E372532B),
    UINT64_C(0xCA273ECEEA26619C), UINT64_C(0xD186B8C721C0C207), UINT64_C(0xEADA7DD6CDE0EB1E), UINT64_C(0xF57D4F7FEE6ED178),
    UINT64_C(0x06F067AA72176FBA), UINT64_C(0x0A637DC5A2C898A6), UINT64_C(0x113F9804BEF90DAE), UINT64_C(0x1B710B35131C471B),
    UINT64_C(0x28DB77F523047D84), UINT64_C(0x32CAAB7B40C72493), UINT64_C(0x3C9EBE0A15C9BEBC), UINT64_C(0x431D67C49C100D4C),
    UINT64_C(0x4CC5D4BECB3E42B6), UINT64_C(0x597F299CFC657E2A), UINT64_C(0x5FCB6FAB3AD6FAEC), UINT64_C(0x6C44198C4A475817),
};

static uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_block(uint64_t h[8], const uint8_t block[128])
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint64_t)block[8 * i] << 56) | ((uint64_t)block[8 * i + 1] << 48)
             | ((uint64_t)block[8 * i + 2] << 40) | ((uint64_t)block[8 * i + 3] << 32)
             | ((uint64_t)block[8 * i + 4] << 24) | ((uint64_t)block[8 * i + 5] << 16)
             | ((uint64_t)block[8 * i + 6] << 8) | (uint64_t)block[8 * i + 7];
    for (int i = 16; i < 80; i++) {
        const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint64_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        const uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        const uint64_t ch = (e & f) ^ (~e & g);
        const uint64_t t1 = hh + S1 + ch + sha512_k[i] + w[i];
        const uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha512_init(sha512_t *s)
{
    memcpy(s->h, sha512_iv, sizeof s->h);
    s->bytes = 0;
    s->fill = 0;
}

static void sha512_update(sha512_t *s, const uint8_t *data, size_t n)
{
    s->bytes += n;
    while (n > 0) {
        if (s->fill == 0 && n >= 128) {
            sha512_block(s->h, data);   // whole blocks straight from the caller's buffer
            data += 128;
            n -= 128;
            continue;
        }
        const size_t take = n < 128 - s->fill ? n : 128 - s->fill;
        memcpy(s->buf + s->fill, data, take);
        s->fill += take;
        data += take;
        n -= take;
        if (s->fill == 128) {
            sha512_block(s->h, s->buf);
            s->fill = 0;
        }
    }
}

static void sha512_final(sha512_t *s, uint8_t out[64])
{
    const uint64_t bits = s->bytes * 8;
    s->buf[s->fill++] = 0x80;
    if (s->fill > 112) {
        memset(s->buf + s->fill, 0, 128 - s->fill);
        sha512_block(s->h, s->buf);
        s->fill = 0;
    }
    memset(s->buf + s->fill, 0, 112 - s->fill);
    // The length field is 128 bits big-endian; every message this code hashes is far
    // below 2^64 bits, so the high half is zero and the low half carries the count.
    memset(s->buf + 112, 0, 8);
    for (int i = 0; i < 8; i++)
        s->buf[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_block(s->h, s->buf);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            out[8 * i + j] = (uint8_t)(s->h[i] >> (56 - 8 * j));
}

// ---------------------------------------------------------------- the field GF(2^255-19)
// 16 signed limbs of radix 2^16 in int64_t. Every operation ends in a carry that keeps
// limbs small enough that the multiply accumulator (16 products per slot, folded once
// more by 38) stays far from int64_t's range.

typedef int64_t gf[16];

static const uint8_t field_p[32] = {   // 2^255-19, little-endian
    0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
};
static const uint8_t exp_p_minus_2[32] = {       // the inversion exponent p-2
    0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
};
static const uint8_t exp_p_plus_3_over_8[32] = { // the square-root exponent (p+3)/8
    0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f,
};
static const uint8_t curve_d[32] = {
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75, 0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c, 0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52,
};
static const uint8_t curve_d2[32] = {            // 2d, as the addition formula wants it
    0x59, 0xf1, 0xb2, 0x26, 0x94, 0x9b, 0xd6, 0xeb, 0x56, 0xb1, 0x83, 0x82, 0x9a, 0x14, 0xe0, 0x00,
    0x30, 0xd1, 0xf3, 0xee, 0xf2, 0x80, 0x8e, 0x19, 0xe7, 0xfc, 0xdf, 0x56, 0xdc, 0xd9, 0x06, 0x24,
};
static const uint8_t sqrt_m1[32] = {             // sqrt(-1) mod p
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4, 0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b, 0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b,
};
static const uint8_t base_x[32] = {
    0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9, 0xb2, 0xa7, 0x25, 0x95, 0x60, 0xc7, 0x2c, 0x69,
    0x5c, 0xdc, 0xd6, 0xfd, 0x31, 0xe2, 0xa4, 0xc0, 0xfe, 0x53, 0x6e, 0xcd, 0xd3, 0x36, 0x69, 0x21,
};
static const uint8_t base_y[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};
static const uint8_t group_order_l[32] = {       // L; a scalar at or above it is rejected
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

static void gf_from_bytes(gf o, const uint8_t s[32])
{
    for (int i = 0; i < 16; i++)
        o[i] = (int64_t)s[2 * i] | ((int64_t)s[2 * i + 1] << 8);
}

static void gf_zero(gf o) { for (int i = 0; i < 16; i++) o[i] = 0; }
static void gf_one(gf o) { gf_zero(o); o[0] = 1; }
static void gf_copy(gf o, const gf a) { for (int i = 0; i < 16; i++) o[i] = a[i]; }

// Normalize into [0, 0xffff] with at most a small positive slack in limb 0. The +2^16
// before each shift keeps the quotient non-negative for the slightly-negative limbs
// subtraction leaves behind; the last limb's carry wraps as 38*(c-1) because
// 2^255 = 19 and the +2^16 round added a spurious 19 on top.
static void gf_carry(gf a)
{
    for (int i = 0; i < 16; i++) {
        a[i] += (int64_t)1 << 16;
        const int64_t c = a[i] >> 16;
        if (i < 15) a[i + 1] += c - 1;
        else        a[0] += 38 * (c - 1);
        a[i] -= c << 16;
    }
}

static void gf_add(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
    gf_carry(o);
}

static void gf_sub(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
    gf_carry(o);
}

static void gf_mul(gf o, const gf a, const gf b)
{
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];    // 2^256 = 2*2^255 = 38 (mod p)
    for (int i = 0; i < 15; i++) {
        t[i + 1] += t[i] >> 16;
        o[i] = t[i] & 0xffff;
    }
    o[15] = t[15] & 0xffff;
    o[0] += 38 * (t[15] >> 16);    // whatever reached 2^256 and above wraps the same way
    gf_carry(o);
}

// o = a^e for a fixed 255-bit exponent given as 32 little-endian bytes: square-and-
// multiply from bit 254 down. The two exponents in play are p-2 (inversion) and
// (p+3)/8 (square root).
static void gf_pow_bytes(gf o, const gf a, const uint8_t e[32])
{
    gf r;
    gf_one(r);
    for (int i = 254; i >= 0; i--) {
        gf_mul(r, r, r);
        if ((e[i >> 3] >> (i & 7)) & 1)
            gf_mul(r, r, a);
    }
    gf_copy(o, r);
}

static void gf_inv(gf o, const gf a) { gf_pow_bytes(o, a, exp_p_minus_2); }

// Canonical little-endian bytes of a field element, fully reduced below p. Points are
// compared by their encodings, so two limb representations of one value must not
// encode differently. The conditional subtract of p runs twice because arriving from
// gf_mul's fold the value can sit below 2p.
static void gf_pack(uint8_t out[32], const gf n)
{
    gf t, m;
    gf_copy(t, n);
    gf_carry(t);
    gf_carry(t);
    gf_carry(t);
    for (int round = 0; round < 2; round++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int underflowed = (int)((m[15] >> 16) & 1);   // 1 when t < p
        m[14] &= 0xffff;
        if (!underflowed) gf_copy(t, m);                    // t was p or more: keep t - p
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i] = (uint8_t)t[i];
        out[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

// 1 when the two elements are the same field value (limbs can differ in representation).
static int gf_equal(const gf a, const gf b)
{
    uint8_t ab[32], bb[32];
    gf_pack(ab, a);
    gf_pack(bb, b);
    return memcmp(ab, bb, 32) == 0;
}

// 1 when the little-endian bytes are strictly below the given modulus.
static int bytes_below(const uint8_t s[32], const uint8_t mod[32])
{
    for (int i = 31; i >= 0; i--) {
        if (s[i] != mod[i]) return s[i] < mod[i];
    }
    return 0;
}

// ---------------------------------------------------------------- curve points
// Extended homogeneous coordinates (X:Y:Z:T), x = X/Z, y = Y/Z, xy = T/Z. The addition
// formula is complete for this curve (d is a non-square), so doubling needs no case of
// its own - double(P) is add(P, P) - and the neutral element needs no special casing.

typedef struct { gf x, y, z, t; } point;

static void point_identity(point *p)
{
    gf_zero(p->x);
    gf_one(p->y);
    gf_one(p->z);
    gf_zero(p->t);
}

static void point_add(point *o, const point *a, const point *b)
{
    gf t1, t2, A, B, C, D, E, F, G, H, d2;
    gf_from_bytes(d2, curve_d2);

    gf_sub(t1, a->y, a->x);        // A = (Y1-X1)*(Y2-X2)
    gf_sub(t2, b->y, b->x);
    gf_mul(A, t1, t2);

    gf_add(t1, a->y, a->x);        // B = (Y1+X1)*(Y2+X2)
    gf_add(t2, b->y, b->x);
    gf_mul(B, t1, t2);

    gf_mul(t1, d2, a->t);          // C = T1*2d*T2
    gf_mul(C, t1, b->t);

    gf_mul(t1, a->z, b->z);        // D = 2*Z1*Z2
    gf_add(D, t1, t1);

    gf_sub(E, B, A);
    gf_sub(F, D, C);
    gf_add(G, D, C);
    gf_add(H, B, A);
    gf_mul(o->x, E, F);
    gf_mul(o->y, G, H);
    gf_mul(o->t, E, H);
    gf_mul(o->z, F, G);
}

// [scalar]P for a little-endian byte string of `n` bytes, MSB-first double-and-add.
// The 64-byte challenge hash is fed unreduced: the group takes it modulo the order
// anyway (an honest public key has order L, so [h]A = [h mod L]A), which only makes
// the check stricter for keys that are not of order L to begin with.
static void point_scalarmult(point *o, const point *p, const uint8_t *scalar, size_t n)
{
    point r;
    point_identity(&r);
    for (int i = (int)(n * 8) - 1; i >= 0; i--) {
        point t;
        point_add(&t, &r, &r);
        r = t;
        if ((scalar[i >> 3] >> (i & 7)) & 1) {
            point t2;
            point_add(&t2, &r, p);
            r = t2;
        }
    }
    *o = r;
}

// The encoding of a point: y with the sign of x in the top bit. Both come out of
// projective arithmetic as arbitrary representatives, so both are canonicalized before
// a single bit is trusted with them.
static void point_pack(uint8_t out[32], const point *p)
{
    gf zinv, x, y;
    uint8_t xb[32];
    gf_inv(zinv, p->z);
    gf_mul(x, p->x, zinv);
    gf_mul(y, p->y, zinv);
    gf_pack(out, y);
    gf_pack(xb, x);
    out[31] |= (uint8_t)((xb[0] & 1) << 7);
}

// Decode a point: recover x from y, matching the sign bit, and refuse anything that is
// not on the curve. This mirrors backend/tools/tatpkg.py's decode, line for line, so the two
// verifiers accept exactly the same byte strings.
static int point_unpack(point *p, const uint8_t s[32])
{
    uint8_t yb[32];
    memcpy(yb, s, 32);
    const int sign = yb[31] >> 7;
    yb[31] &= 0x7f;
    if (!bytes_below(yb, field_p))
        return 0;

    gf y, u, v, t1, x;
    gf_from_bytes(y, yb);

    gf_mul(u, y, y);               // u = y^2 - 1
    gf_one(t1);
    gf_sub(u, u, t1);

    gf_mul(v, y, y);               // v = d*y^2 + 1
    gf_from_bytes(t1, curve_d);
    gf_mul(v, v, t1);
    gf_one(t1);
    gf_add(v, v, t1);

    gf_inv(t1, v);                 // xx = u/v; x = xx^((p+3)/8), times sqrt(-1) if that
    gf_mul(u, u, t1);              // missed the square
    gf_pow_bytes(x, u, exp_p_plus_3_over_8);
    gf_mul(t1, x, x);
    if (!gf_equal(t1, u)) {
        gf_from_bytes(t1, sqrt_m1);
        gf_mul(x, x, t1);
    }
    {
        uint8_t xb[32];            // the sign convention is about the canonical value;
        gf_pack(xb, x);            // a representative above p has the wrong low bit
        if ((xb[0] & 1) != sign) {
            gf_zero(t1);
            gf_sub(x, t1, x);      // x = -x
        }
    }

    gf xx, yy, chk;                // the curve equation as one final gate
    gf_mul(xx, x, x);
    gf_mul(yy, y, y);
    gf_mul(chk, xx, yy);
    gf_from_bytes(t1, curve_d);
    gf_mul(chk, chk, t1);
    gf_add(chk, chk, xx);
    gf_sub(chk, yy, chk);
    gf_one(t1);
    gf_sub(chk, chk, t1);
    gf_zero(t1);
    if (!gf_equal(chk, t1))
        return 0;

    gf_copy(p->x, x);
    gf_copy(p->y, y);
    gf_one(p->z);
    gf_mul(p->t, x, y);
    return 1;
}

// ---------------------------------------------------------------- the check itself

// s*B == R + h*A, the RFC 8032 non-cofactored equation, with the canonicality gates
// (S below the group order, both points decodable) that malleable forgeries trip.
static int ed25519_check(const uint8_t R[32], const uint8_t S[32], const uint8_t A[32],
                         const uint8_t h[64])
{
    if (!bytes_below(S, group_order_l))
        return 0;
    point a_pt, r_pt, b_pt, left, right, t;
    if (!point_unpack(&a_pt, A) || !point_unpack(&r_pt, R))
        return 0;
    gf_from_bytes(b_pt.x, base_x);
    gf_from_bytes(b_pt.y, base_y);
    gf_one(b_pt.z);
    gf_mul(b_pt.t, b_pt.x, b_pt.y);

    point_scalarmult(&left, &b_pt, S, 32);
    point_scalarmult(&t, &a_pt, h, 64);
    point_add(&right, &r_pt, &t);
    uint8_t lb[32], rb[32];
    point_pack(lb, &left);
    point_pack(rb, &right);
    return memcmp(lb, rb, 32) == 0;
}

// ---------------------------------------------------------------- the package

static uint32_t rd32(const uint8_t *b)
{
    return b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// What the section table says about the SIG section: 1 found, 0 none (an unsigned
// package), -1 broken in a way that touches the signature story. `head` must cover
// the header and the whole table; `file_len` is the whole package.
static int find_sig(const uint8_t *head, size_t head_have, uint64_t file_len, size_t *sig_off)
{
    if (head_have < SIG_HEADER_BYTES || memcmp(head, "TATPKG\0\x01", 8) != 0)
        return -1;
    if ((uint64_t)rd32(head + 12) != file_len)
        return -1;
    const unsigned count = head[92] | ((unsigned)head[93] << 8);
    if (count == 0 || count > SIG_MAX_SECTIONS)
        return -1;
    const size_t table_end = SIG_HEADER_BYTES + count * SIG_TABLE_ENTRY;
    if (head_have < table_end)
        return -1;

    int found = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = head + SIG_HEADER_BYTES + i * SIG_TABLE_ENTRY;
        if (memcmp(e, "SIG ", 4) != 0)
            continue;
        if (found)
            return -1;             // two signatures is not a state a packer produces
        const uint64_t off = rd32(e + 4), size = rd32(e + 8);
        if (size != SIG_SECTION_BYTES || off < table_end || off + size != file_len)
            return -1;             // the signature covers everything before it: it is last
        *sig_off = (size_t)off;
        found = 1;
    }
    return found;
}

// The signed prefix reaches the hash in chunks: whole in memory, or a few KB at a time
// from an open file - a 4 MB package is never held whole just to verify it.
typedef int (*prefix_feed_t)(sha512_t *sha, void *ctx);

typedef struct { const uint8_t *buf; size_t off; } mem_feed_t;

static int feed_mem(sha512_t *sha, void *ctx)
{
    mem_feed_t *m = ctx;
    sha512_update(sha, m->buf, m->off);
    return 1;
}

typedef struct { FILE *f; size_t off; } file_feed_t;

static int feed_file(sha512_t *sha, void *ctx)
{
    file_feed_t *m = ctx;
    uint8_t chunk[SIG_FILE_CHUNK];
    size_t done = 0;
    while (done < m->off) {
        const size_t want = m->off - done > sizeof chunk ? sizeof chunk : m->off - done;
        if (fread(chunk, 1, want, m->f) != want)
            return 0;
        sha512_update(sha, chunk, want);
        done += want;
    }
    return 1;
}

// Hash R, the public key and the signed prefix into the challenge, then run the point
// check. `sig` points at the section payload: key id, R, S.
static loader_sig_t verdict(const uint8_t sig[SIG_SECTION_BYTES], prefix_feed_t feed, void *ctx)
{
    const uint8_t *R = sig + 1, *S = sig + 33;
    const uint8_t *A = NULL;
    for (size_t i = 0; i < loader_signing_key_count; i++)
        if (loader_signing_keys[i].key_id == sig[0])
            A = loader_signing_keys[i].public_key;
    if (!A)
        return LOADER_SIG_INVALID;  // a key this build does not know

    sha512_t sha;
    sha512_init(&sha);
    sha512_update(&sha, R, 32);
    sha512_update(&sha, A, 32);
    if (!feed(&sha, ctx))
        return LOADER_SIG_INVALID;
    uint8_t h[64];
    sha512_final(&sha, h);
    return ed25519_check(R, S, A, h) ? LOADER_SIG_VALID : LOADER_SIG_INVALID;
}

loader_sig_t loader_verify_signature(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    const int r = find_sig(buf, len, len, &off);
    if (r < 0)
        return LOADER_SIG_INVALID;
    if (r == 0)
        return LOADER_SIG_ABSENT;
    mem_feed_t m = { buf, off };
    return verdict(buf + off, feed_mem, &m);
}

loader_sig_t loader_verify_signature_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return LOADER_SIG_INVALID;

    loader_sig_t out = LOADER_SIG_INVALID;
    uint8_t *head = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long len = ftell(f);
        if (len > 0 && fseek(f, 0, SEEK_SET) == 0) {
            head = malloc(SIG_HEADER_BYTES);
            if (head && fread(head, 1, SIG_HEADER_BYTES, f) == SIG_HEADER_BYTES) {
                const unsigned count = head[92] | ((unsigned)head[93] << 8);
                const size_t table_end = SIG_HEADER_BYTES +
                                         (count <= SIG_MAX_SECTIONS ? count : 1) * SIG_TABLE_ENTRY;
                uint8_t *full = realloc(head, table_end);
                if (full) {
                    head = full;
                    if (fread(head + SIG_HEADER_BYTES, 1, table_end - SIG_HEADER_BYTES, f)
                            == table_end - SIG_HEADER_BYTES) {
                        size_t off = 0;
                        const int r = find_sig(head, table_end, (uint64_t)len, &off);
                        if (r == 0)
                            out = LOADER_SIG_ABSENT;
                        else if (r > 0) {
                            uint8_t sig[SIG_SECTION_BYTES];
                            if (fseek(f, (long)off, SEEK_SET) == 0 &&
                                fread(sig, 1, sizeof sig, f) == sizeof sig &&
                                fseek(f, 0, SEEK_SET) == 0) {
                                file_feed_t m = { f, off };
                                out = verdict(sig, feed_file, &m);
                            }
                        }
                    }
                }
            }
        }
    }
    free(head);
    fclose(f);
    return out;
}
