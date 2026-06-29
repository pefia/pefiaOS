/* kernel/crypto.h
 * -----------------------------------------------------------------------------
 * Just enough cryptography to drive a TLS 1.3 client:
 *   SHA-256, HMAC-SHA256, HKDF (+ TLS HKDF-Expand-Label), AES-128-GCM, X25519.
 * Hand-written, freestanding, not constant-time / not hardened — appropriate
 * for a hobby OS reaching the web, not for protecting secrets.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_CRYPTO_H
#define PEFIA_CRYPTO_H

#include <stdint.h>

/* ---- SHA-256 ---- */
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

/* ---- HMAC / HKDF (SHA-256) ---- */
void hmac_sha256(const uint8_t *key, int klen, const uint8_t *msg, int mlen, uint8_t out[32]);
void hkdf_extract(const uint8_t *salt, int slen, const uint8_t *ikm, int ilen, uint8_t out[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, int ilen, uint8_t *out, int outlen);
/* TLS 1.3 HKDF-Expand-Label with the "tls13 " prefix. */
void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *ctx, int ctxlen, uint8_t *out, int outlen);

/* ---- AES-128 / AES-128-GCM ---- */
typedef struct { uint8_t rk[176]; } aes128_ctx;
void aes128_init(aes128_ctx *c, const uint8_t key[16]);
void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);

/* AEAD seal/open. nonce is 12 bytes. tag is 16 bytes. Returns 0 on success
 * (open returns <0 if the tag does not verify). */
int aesgcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, int aadlen,
                const uint8_t *pt, int len, uint8_t *ct, uint8_t tag[16]);
int aesgcm_open(const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, int aadlen,
                const uint8_t *ct, int len, const uint8_t tag[16], uint8_t *pt);

/* ---- X25519 ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* ---- weak randomness (rdtsc-seeded xorshift) ---- */
void rng_bytes(uint8_t *out, int n);

#endif /* PEFIA_CRYPTO_H */
