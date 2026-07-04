#include "crypto.h"
#include "clock.h"

/* Freestanding build, no libc, so the usual mem* trio gets reinvented here
 * in miniature. Nothing fancy - these just need to be correct, not fast. */
static void bcopy_(void *d, const void *s, int n) { uint8_t *a = d; const uint8_t *b = s; for (int i = 0; i < n; i++) a[i] = b[i]; }
static void bzero_(void *d, int c, int n) { uint8_t *a = d; for (int i = 0; i < n; i++) a[i] = (uint8_t)c; }
static int  slen_(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ---------------------------------------------------------------------
 * SHA-256 (FIPS 180-4)
 * --------------------------------------------------------------------- */

/* fractional bits of cube roots of the first 64 primes - standard table,
 * do not touch. */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

/* Compresses one 64-byte block into the running state. This is the whole
 * algorithm - message schedule expansion followed by 64 mixing rounds. */
static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void sha256_init(sha256_ctx *c)
{
    /* first 32 bits of the fractional parts of sqrt(2..19) */
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->total = 0; c->idx = 0;
}

void sha256_update(sha256_ctx *c, const void *data, int len)
{
    const uint8_t *p = data;
    c->total += (uint64_t)len;
    while (len > 0) {
        int take = 64 - c->idx;
        if (take > len) take = len;
        bcopy_(c->buf + c->idx, p, take);
        c->idx += take; p += take; len -= take;
        if (c->idx == 64) { sha256_block(c, c->buf); c->idx = 0; }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    /* Merkle-Damgard padding: one 1-bit, zeros, then the 64-bit bit length. */
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->idx != 56) sha256_update(c, &z, 1);
    uint8_t lenbytes[8];
    for (int i = 0; i < 8; i++) lenbytes[i] = (uint8_t)(bits >> (56 - i*8));
    sha256_update(c, lenbytes, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

void sha256(const void *data, int len, uint8_t out[32])
{
    sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ---------------------------------------------------------------------
 * HMAC-SHA256 / HKDF (RFC 2104, RFC 5869)
 * --------------------------------------------------------------------- */

void hmac_sha256(const uint8_t *key, int klen, const uint8_t *msg, int mlen, uint8_t out[32])
{
    uint8_t k[64], ipad[64], opad[64], inner[32];
    bzero_(k, 0, 64);
    /* keys longer than the block size get hashed down first, per spec */
    if (klen > 64) { sha256(key, klen, k); }
    else bcopy_(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    sha256_ctx c;
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

void hkdf_extract(const uint8_t *salt, int slen, const uint8_t *ikm, int ilen, uint8_t out[32])
{
    /* RFC 5869 says an absent salt is treated as HashLen zero bytes */
    uint8_t zero[32];
    if (!salt || slen == 0) { bzero_(zero, 0, 32); salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, out);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t *info, int ilen, uint8_t *out, int outlen)
{
    uint8_t t[32]; int tlen = 0, done = 0; uint8_t counter = 1;
    while (done < outlen) {
        /* T(n) = HMAC(PRK, T(n-1) || info || n) - the 256 headroom is just
         * more than any info string this codebase ever builds. */
        uint8_t chunk[32 + 256 + 1];
        int n = 0;
        bcopy_(chunk, t, tlen); n += tlen;
        bcopy_(chunk + n, info, ilen); n += ilen;
        chunk[n++] = counter++;
        hmac_sha256(prk, 32, chunk, n, t);
        tlen = 32;
        int take = outlen - done; if (take > 32) take = 32;
        bcopy_(out + done, t, take);
        done += take;
    }
}

void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *ctx, int ctxlen, uint8_t *out, int outlen)
{
    /* Builds the HkdfLabel struct straight from RFC 8446 section 7.1:
     * uint16 length, opaque label<7..255> (with the "tls13 " prefix baked
     * in), opaque context<0..255>. */
    uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
    int n = 0;
    info[n++] = (uint8_t)(outlen >> 8);
    info[n++] = (uint8_t)outlen;
    int llen = slen_(label);
    info[n++] = (uint8_t)(6 + llen);
    bcopy_(info + n, "tls13 ", 6); n += 6;
    bcopy_(info + n, label, llen); n += llen;
    info[n++] = (uint8_t)ctxlen;
    if (ctxlen > 0) { bcopy_(info + n, ctx, ctxlen); n += ctxlen; }
    hkdf_expand(secret, info, n, out, outlen);
}

/* ---------------------------------------------------------------------
 * AES-128
 * --------------------------------------------------------------------- */

static const uint8_t SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static const uint8_t RCON[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

/* Key expansion: 11 round keys (176 bytes) from the 16-byte AES-128 key. */
void aes128_init(aes128_ctx *c, const uint8_t key[16])
{
    uint8_t *rk = c->rk;
    bcopy_(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t word[4];
        bcopy_(word, rk + (i-1)*4, 4);
        if (i % 4 == 0) {
            uint8_t rotated0 = word[0];
            word[0] = SBOX[word[1]] ^ RCON[i/4 - 1];
            word[1] = SBOX[word[2]];
            word[2] = SBOX[word[3]];
            word[3] = SBOX[rotated0];
        }
        for (int j = 0; j < 4; j++) rk[i*4 + j] = rk[(i-4)*4 + j] ^ word[j];
    }
}

/* GF(2^8) multiply-by-2 (the "xtime" op from FIPS 197) and general
 * multiply built from it via the standard double-and-add. */
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}

void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16])
{
    uint8_t state[16];
    const uint8_t *rk = c->rk;
    for (int i = 0; i < 16; i++) state[i] = in[i] ^ rk[i];

    for (int round = 1; round <= 10; round++) {
        uint8_t sub[16];
        for (int i = 0; i < 16; i++) sub[i] = SBOX[state[i]];
        /* ShiftRows, applied directly into the shuffled layout rather than
         * rotating rows in place - saves a temp buffer */
        uint8_t shifted[16];
        shifted[0]=sub[0];  shifted[4]=sub[4];  shifted[8]=sub[8];   shifted[12]=sub[12];
        shifted[1]=sub[5];  shifted[5]=sub[9];  shifted[9]=sub[13];  shifted[13]=sub[1];
        shifted[2]=sub[10]; shifted[6]=sub[14]; shifted[10]=sub[2];  shifted[14]=sub[6];
        shifted[3]=sub[15]; shifted[7]=sub[3];  shifted[11]=sub[7];  shifted[15]=sub[11];

        if (round != 10) {
            /* MixColumns - skipped on the last round per the AES spec */
            for (int col = 0; col < 4; col++) {
                uint8_t *p = shifted + col*4;
                uint8_t c0=p[0],c1=p[1],c2=p[2],c3=p[3];
                p[0] = (uint8_t)(gmul(c0,2) ^ gmul(c1,3) ^ c2 ^ c3);
                p[1] = (uint8_t)(c0 ^ gmul(c1,2) ^ gmul(c2,3) ^ c3);
                p[2] = (uint8_t)(c0 ^ c1 ^ gmul(c2,2) ^ gmul(c3,3));
                p[3] = (uint8_t)(gmul(c0,3) ^ c1 ^ c2 ^ gmul(c3,2));
            }
        }
        for (int i = 0; i < 16; i++) state[i] = shifted[i] ^ rk[round*16 + i];
    }
    bcopy_(out, state, 16);
}

/* ---------------------------------------------------------------------
 * AES-128-GCM
 * --------------------------------------------------------------------- */

/* Multiplies X by H in GF(2^128) using the reduction polynomial from
 * NIST SP 800-38D (bit-by-bit shift-and-reduce - not remotely
 * constant-time, but the tag isn't a scalar with much timing signal to
 * begin with, so we didn't bother with a table-driven version). */
static void gf_mul(uint8_t *X, const uint8_t *H)
{
    uint8_t acc[16], v[16];
    bzero_(acc, 0, 16);
    bcopy_(v, H, 16);
    for (int i = 0; i < 128; i++) {
        int bit = (X[i >> 3] >> (7 - (i & 7))) & 1;
        if (bit) for (int j = 0; j < 16; j++) acc[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | ((v[j-1] & 1) << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    bcopy_(X, acc, 16);
}

/* GHASH over AAD then ciphertext, each zero-padded to a 16-byte multiple,
 * finished off with the 64/64-bit length block. */
static void ghash(const uint8_t H[16], const uint8_t *aad, int aadlen,
                  const uint8_t *ct, int ctlen, uint8_t out[16])
{
    uint8_t y[16];
    bzero_(y, 0, 16);
    int i;
    for (i = 0; i + 16 <= aadlen; i += 16) { for (int j = 0; j < 16; j++) y[j] ^= aad[i+j]; gf_mul(y, H); }
    if (i < aadlen) { for (int j = 0; j < aadlen - i; j++) y[j] ^= aad[i+j]; gf_mul(y, H); }
    for (i = 0; i + 16 <= ctlen; i += 16) { for (int j = 0; j < 16; j++) y[j] ^= ct[i+j]; gf_mul(y, H); }
    if (i < ctlen) { for (int j = 0; j < ctlen - i; j++) y[j] ^= ct[i+j]; gf_mul(y, H); }
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)ctlen * 8;
    uint8_t lenblock[16];
    for (int j = 0; j < 8; j++) lenblock[j]   = (uint8_t)(abits >> (56 - j*8));
    for (int j = 0; j < 8; j++) lenblock[8+j] = (uint8_t)(cbits >> (56 - j*8));
    for (int j = 0; j < 16; j++) y[j] ^= lenblock[j];
    gf_mul(y, H);
    bcopy_(out, y, 16);
}

/* GCM only ever increments the low 32 bits of the counter block. */
static void inc32(uint8_t *ctr) { for (int i = 15; i >= 12; i--) { if (++ctr[i]) break; } }

/* CTR-mode keystream XOR, starting from J0+1 (J0 itself is reserved for
 * masking the tag). */
static void gcm_crypt(const aes128_ctx *aes, const uint8_t J0[16],
                      const uint8_t *in, int len, uint8_t *out)
{
    uint8_t ctr[16], ks[16];
    bcopy_(ctr, J0, 16);
    for (int off = 0; off < len; off += 16) {
        inc32(ctr);
        aes128_encrypt(aes, ctr, ks);
        int n = len - off; if (n > 16) n = 16;
        for (int j = 0; j < n; j++) out[off+j] = in[off+j] ^ ks[j];
    }
}

/* Shared setup for seal/open: derive the hash subkey H = AES_K(0) and the
 * initial counter block J0 from the 96-bit nonce (the "96-bit IV" case in
 * SP 800-38D, which is the only one TLS 1.3 uses). */
static void gcm_prep(const uint8_t key[16], const uint8_t nonce[12],
                     aes128_ctx *aes, uint8_t H[16], uint8_t J0[16])
{
    aes128_init(aes, key);
    uint8_t zero[16]; bzero_(zero, 0, 16);
    aes128_encrypt(aes, zero, H);
    bcopy_(J0, nonce, 12);
    J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1;
}

int aesgcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, int aadlen,
                const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16])
{
    aes128_ctx aes; uint8_t H[16], J0[16], S[16], ej0[16];
    gcm_prep(key, nonce, &aes, H, J0);
    gcm_crypt(&aes, J0, pt, len, ct);
    ghash(H, aad, aadlen, ct, len, S);
    aes128_encrypt(&aes, J0, ej0);
    for (int i = 0; i < 16; i++) tag[i] = S[i] ^ ej0[i];
    return 0;
}

int aesgcm_open(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, int aadlen,
                const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt)
{
    aes128_ctx aes; uint8_t H[16], J0[16], S[16], ej0[16], expect[16];
    gcm_prep(key, nonce, &aes, H, J0);
    ghash(H, aad, aadlen, ct, len, S);
    aes128_encrypt(&aes, J0, ej0);
    for (int i = 0; i < 16; i++) expect[i] = S[i] ^ ej0[i];
    /* OR the diffs together instead of an early-exit compare - a bailout
     * on the first mismatched byte would leak how much of the tag was
     * right via timing, which somewhat defeats the point of a MAC. */
    int diff = 0;
    for (int i = 0; i < 16; i++) diff |= expect[i] ^ tag[i];
    if (diff) return -1;
    gcm_crypt(&aes, J0, ct, len, pt);
    return 0;
}

/* ---------------------------------------------------------------------
 * X25519 - ported from the field-arithmetic core of public-domain
 * TweetNaCl. The variable names below (gf, A/Z/M/S, car25519, ...) are
 * kept close to the original so it's still recognizable against a
 * reference implementation if something ever needs auditing.
 * --------------------------------------------------------------------- */

typedef long long i64;
typedef i64 gf[16];                 /* field element: 16 limbs, 16 bits each */
static const gf _121665 = {0xDB41, 1};   /* the curve constant (a-2)/4 */

/* Carry propagation across the 16 limbs, with the final limb folded back
 * around mod 2^255-19 (the "* 37" term is 19*2 from the doubled carry-out
 * at the top limb). */
static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        i64 c = o[i] >> 16;
        o[(i+1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

/* Branchless conditional swap: b must be 0 or 1. Used so the Montgomery
 * ladder doesn't take a data-dependent branch on the secret scalar bit. */
static void sel25519(gf p, gf q, int b)
{
    i64 mask = ~(b - 1);
    for (int i = 0; i < 16; i++) { i64 t = mask & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

/* Reduces mod 2^255-19 and serializes to 32 little-endian bytes. */
static void pack25519(uint8_t *o, const gf n)
{
    gf cand, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        cand[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { cand[i] = t[i] - 0xffff - ((cand[i-1] >> 16) & 1); cand[i-1] &= 0xffff; }
        cand[15] = t[15] - 0x7fff - ((cand[14] >> 16) & 1);
        int borrow = (cand[15] >> 16) & 1;
        cand[14] &= 0xffff;
        sel25519(t, cand, 1 - borrow);
    }
    for (int i = 0; i < 16; i++) { o[2*i] = (uint8_t)(t[i] & 0xff); o[2*i+1] = (uint8_t)(t[i] >> 8); }
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((i64)n[2*i+1] << 8);
    o[15] &= 0x7fff;    /* clamp to 255 bits */
}

static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

/* Schoolbook multiply across all 16x16 limb products, then fold the high
 * half back in using 38 = 2*19 (since 2^256 = 38 * 2^251, mod 2^255-19). */
static void M(gf o, const gf a, const gf b)
{
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

/* Fermat inversion: a^(p-2) mod p via square-and-multiply over the fixed
 * bit pattern of p-2 = 2^255-21. Constant-time in the sense that it always
 * does the same sequence of squarings/multiplies regardless of the input. */
static void inv25519(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t clamped[32];
    i64 work[80];
    gf x2, x3, z2, z3, tmp0, tmp1;

    /* RFC 7748 clamping - forces the scalar into the cofactor-safe,
     * high-bit-fixed range the ladder below expects. */
    for (int i = 0; i < 31; i++) clamped[i] = scalar[i];
    clamped[31] = (scalar[31] & 127) | 64;
    clamped[0] &= 248;

    unpack25519(work, point);
    for (int i = 0; i < 16; i++) { x3[i] = work[i]; z3[i] = x2[i] = z2[i] = 0; }
    x2[0] = z3[0] = 1;

    /* Montgomery ladder, one step per scalar bit from the top down. Swap
     * order is chosen entirely by the bit value so there's no branch on
     * secret data - just arithmetic that happens to no-op half the time. */
    for (int i = 254; i >= 0; --i) {
        i64 bit = (clamped[i >> 3] >> (i & 7)) & 1;
        sel25519(x2, x3, bit); sel25519(z2, z3, bit);
        A(tmp0, x2, z2); Z(x2, x2, z2); A(z2, x3, z3); Z(x3, x3, z3);
        S(z3, tmp0); S(tmp1, x2); M(x2, z2, x2); M(z2, x3, tmp0);
        A(tmp0, x2, z2); Z(x2, x2, z2); S(x3, x2); Z(z2, z3, tmp1);
        M(x2, z2, _121665); A(x2, x2, z3); M(z2, z2, x2); M(x2, z3, tmp1);
        M(z3, x3, work); S(x3, tmp0); sel25519(x2, x3, bit); sel25519(z2, z3, bit);
    }

    for (int i = 0; i < 16; i++) { work[i+16] = x2[i]; work[i+32] = z2[i]; work[i+48] = x3[i]; work[i+64] = z3[i]; }
    inv25519(work + 32, work + 32);
    M(work + 16, work + 16, work + 32);
    pack25519(out, work + 16);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    uint8_t base[32]; bzero_(base, 0, 32); base[0] = 9;   /* the standard Curve25519 base point */
    x25519(out, scalar, base);
}

/* ---------------------------------------------------------------------
 * Weak RNG
 *
 * There's no hardware RNG driver and no entropy pool in this kernel, so
 * this is an rdtsc-seeded xorshift64. It's fine for picking ephemeral
 * X25519 scalars and GCM nonces on a single-user hobby box; it would be a
 * liability anywhere an attacker could predict or influence timing.
 * --------------------------------------------------------------------- */

static uint64_t g_rng_state = 0;

void rng_bytes(uint8_t *out, int n)
{
    if (g_rng_state == 0) g_rng_state = rdtsc() | 1;
    for (int i = 0; i < n; i++) {
        g_rng_state ^= rdtsc();
        uint64_t x = g_rng_state;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        g_rng_state = x;
        out[i] = (uint8_t)(x >> 24);
    }
}
