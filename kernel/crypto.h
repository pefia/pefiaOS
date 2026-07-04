/* kernel/crypto.h
 *
 * The crypto primitives needed to speak TLS 1.3 to a real web server:
 * SHA-256, HMAC/HKDF on top of it, AES-128-GCM, and X25519.
 *
 * None of this is hardened. No constant-time guarantees, no side-channel
 * hygiene, no attempt to zero secrets after use. It's here so the browser
 * can fetch pages over HTTPS, not to keep anyone's keys safe from a
 * determined attacker with a cache-timing rig. Treat it accordingly.
 */
#ifndef PEFIA_CRYPTO_H
#define PEFIA_CRYPTO_H

#include <stdint.h>

/* SHA-256 */
typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    int      idx;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, int len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, int len, uint8_t out[32]);

/* HMAC-SHA256 and the two HKDF halves, plus the TLS 1.3 flavor of Expand
 * that folds in the "tls13 " label prefix. */
void hmac_sha256(const uint8_t *key, int klen, const uint8_t *msg, int mlen, uint8_t out[32]);
void hkdf_extract(const uint8_t *salt, int slen, const uint8_t *ikm, int ilen, uint8_t out[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, int ilen, uint8_t *out, int outlen);
void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *ctx, int ctxlen, uint8_t *out, int outlen);

/* AES-128 block cipher and the GCM mode built on it. */
typedef struct { uint8_t rk[176]; } aes128_ctx;
void aes128_init(aes128_ctx *c, const uint8_t key[16]);
void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);

/* nonce is 12 bytes, tag is 16. aesgcm_open returns <0 if the tag doesn't
 * match (which also means: don't trust anything it wrote to pt). */
int aesgcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, int aadlen,
                const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int aesgcm_open(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, int aadlen,
                const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);

/* X25519 key agreement. x25519_base is just x25519 against the standard
 * base point, split out because it's what you call for your own keypair. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* Not a CSPRNG - xorshift seeded off rdtsc. Fine for nonces and ephemeral
 * X25519 scalars on a machine with nothing better to draw entropy from. */
void rng_bytes(uint8_t *out, int n);

#endif /* PEFIA_CRYPTO_H */
