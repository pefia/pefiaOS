#include "tls.h"
#include "crypto.h"
#include "netstack.h"
#include "clock.h"

/* no libc here, so the usual trio gets reinvented locally */
static void copy_bytes(void *d, const void *s, int n) { uint8_t *a=d; const uint8_t *b=s; for(int i=0;i<n;i++) a[i]=b[i]; }
static void fill_bytes(void *d, int c, int n) { uint8_t *a=d; for(int i=0;i<n;i++) a[i]=(uint8_t)c; }
static int  str_len(const char *s){int n=0;while(s[n])n++;return n;}

static char g_status[64];
static void set_status(const char *s){ int i=0; while(s[i]&&i<63){g_status[i]=s[i];i++;} g_status[i]=0; }
const char *tls_status(void){ return g_status; }

/* ==========================================================================
 * connection state - one connection at a time, all static storage. A real
 * client would carry this in a struct and support several connections;
 * this OS only ever needs one at a time, so globals keep it simple.
 * ========================================================================== */

#define IN_CAP   33000   /* raw TCP staging buffer */
#define HS_CAP   20000   /* reassembled handshake message buffer */
#define DEC_CAP  17000   /* largest plaintext record we'll produce/consume */

static int      g_open = 0;
static uint8_t  g_in[IN_CAP];
static int      g_in_off, g_in_len;

static sha256_ctx g_th;          /* transcript hash, updated as messages arrive/are sent */

static uint8_t  g_priv[32], g_pub[32], g_crandom[32], g_session[32];

static uint8_t  g_hs_secret[32];
static uint8_t  g_c_hs[32], g_s_hs[32];
static uint8_t  g_c_hs_key[16], g_c_hs_iv[12];
static uint8_t  g_s_hs_key[16], g_s_hs_iv[12];
static uint64_t g_c_hs_seq, g_s_hs_seq;

static uint8_t  g_c_ap[32], g_s_ap[32];
static uint8_t  g_c_ap_key[16], g_c_ap_iv[12];
static uint8_t  g_s_ap_key[16], g_s_ap_iv[12];
static uint64_t g_c_ap_seq, g_s_ap_seq;

/* decrypted application data the caller hasn't picked up with tls_recv yet */
static uint8_t  g_appbuf[DEC_CAP];
static int      g_app_off, g_app_len;

/* ==========================================================================
 * TCP staging + TLS record framing
 * ========================================================================== */

/* Makes sure at least `need` bytes are sitting in g_in[g_in_off..], pulling
 * more off the socket and compacting the buffer as needed. TLS records
 * don't line up with TCP segments, so this is what lets read_record()
 * pretend they do. */
static int stage_fill(int need, int timeout_ms)
{
    uint32_t t0 = clock_ms();
    while (g_in_len - g_in_off < need) {
        if (g_in_off > 0) {
            int rem = g_in_len - g_in_off;
            for (int i = 0; i < rem; i++) g_in[i] = g_in[g_in_off + i];
            g_in_len = rem; g_in_off = 0;
        }
        int space = IN_CAP - g_in_len;
        if (space <= 0) return -1;      /* one record shouldn't ever need this much */
        int n = tcp_recv(g_in + g_in_len, space, 300);
        if (n > 0) { g_in_len += n; continue; }
        if (tcp_closed()) return -1;
        if ((uint32_t)(clock_ms() - t0) >= (uint32_t)timeout_ms) return -1;
    }
    return 0;
}

static uint8_t  g_rec_hdr[5];
static uint8_t *g_rec_payload;
static int      g_rec_len, g_rec_type;

/* Pulls one TLS record (5-byte header + payload) into g_rec_*. The payload
 * pointer aliases straight into g_in, so it stays valid only until the
 * next read_record() call. */
static int read_record(int timeout_ms)
{
    if (stage_fill(5, timeout_ms)) return -1;
    uint8_t *h = g_in + g_in_off;
    g_rec_type = h[0];
    int len = (h[3] << 8) | h[4];
    if (len < 0 || len > IN_CAP - 5) return -1;
    if (stage_fill(5 + len, timeout_ms)) return -1;
    copy_bytes(g_rec_hdr, g_in + g_in_off, 5);
    g_rec_payload = g_in + g_in_off + 5;
    g_rec_len = len;
    g_in_off += 5 + len;
    return 0;
}

/* ==========================================================================
 * key schedule (RFC 8446 section 7.1)
 * ========================================================================== */

static void th_snapshot(uint8_t out[32]) { sha256_ctx t = g_th; sha256_final(&t, out); }

static void derive_secret(const uint8_t secret[32], const char *label,
                          const uint8_t thash[32], uint8_t out[32])
{
    hkdf_expand_label(secret, label, thash, 32, out, 32);
}

static void derive_keyiv(const uint8_t secret[32], uint8_t key[16], uint8_t iv[12])
{
    hkdf_expand_label(secret, "key", 0, 0, key, 16);
    hkdf_expand_label(secret, "iv", 0, 0, iv, 12);
}

/* per-record nonce = static IV XOR the big-endian sequence number */
static void make_nonce(const uint8_t iv[12], uint64_t seq, uint8_t out[12])
{
    copy_bytes(out, iv, 12);
    for (int i = 0; i < 8; i++) out[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* ==========================================================================
 * outbound records - wraps plaintext in the TLSInnerPlaintext envelope
 * (content || inner type || zero padding, here always zero padding bytes)
 * and seals it as an AES-GCM "application_data" record.
 * ========================================================================== */

static int send_encrypted(int inner_type, const uint8_t *data, int len,
                          const uint8_t key[16], const uint8_t iv[12], uint64_t *seq)
{
    static uint8_t inner[DEC_CAP];
    static uint8_t rec[DEC_CAP + 32];
    if (len + 1 > DEC_CAP) return -1;
    copy_bytes(inner, data, len);
    inner[len] = (uint8_t)inner_type;
    int ilen = len + 1;

    int reclen = ilen + 16;   /* + GCM tag */
    rec[0] = 23; rec[1] = 0x03; rec[2] = 0x03;
    rec[3] = (uint8_t)(reclen >> 8); rec[4] = (uint8_t)reclen;

    uint8_t nonce[12];
    make_nonce(iv, *seq, nonce);
    aesgcm_seal(key, nonce, rec, 5, inner, ilen, rec + 5, rec + 5 + ilen);
    (*seq)++;

    return tcp_send(rec, 5 + reclen) < 0 ? -1 : 0;
}

/* Unseals the record currently sitting in g_rec_* into g_dec. Returns the
 * inner content type on success, or a negative code:
 *   -1 ciphertext shorter than the tag, -2 tag mismatch, -3 all-zero body
 * (which would mean no inner type byte survived the padding strip). */
static uint8_t g_dec[DEC_CAP];
static int g_dec_len;

static int decrypt_record(const uint8_t key[16], const uint8_t iv[12], uint64_t *seq)
{
    int ctlen = g_rec_len - 16;
    if (ctlen < 0) return -1;
    uint8_t nonce[12];
    make_nonce(iv, *seq, nonce);
    if (aesgcm_open(key, nonce, g_rec_hdr, 5, g_rec_payload, ctlen,
                    g_rec_payload + ctlen, g_dec) != 0) return -2;
    (*seq)++;
    int n = ctlen;
    while (n > 0 && g_dec[n - 1] == 0) n--;   /* strip the zero padding TLS 1.3 allows */
    if (n == 0) return -3;
    g_dec_len = n - 1;
    return g_dec[n - 1];                        /* the real content type, from the last byte */
}

/* ==========================================================================
 * ClientHello construction
 * ========================================================================== */

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static int build_client_hello(uint8_t *out, const char *host)
{
    int hlen = str_len(host);
    uint8_t body[512];
    int b = 0;

    body[b++] = 0x03; body[b++] = 0x03;            /* legacy_version, always TLS 1.2 here */
    copy_bytes(body + b, g_crandom, 32); b += 32;
    body[b++] = 32; copy_bytes(body + b, g_session, 32); b += 32; /* legacy session id, unused by 1.3 but servers expect it */
    body[b++] = 0x00; body[b++] = 0x02;
    body[b++] = 0x13; body[b++] = 0x01;            /* the one cipher suite we speak: TLS_AES_128_GCM_SHA256 */
    body[b++] = 0x01; body[b++] = 0x00;            /* compression methods: null only */

    /* extensions block - length gets patched in once we know the size */
    int ext_len_pos = b; b += 2;
    int ext_start = b;

    /* server_name (SNI) - the whole reason we need `host` here at all */
    put16(body + b, 0x0000); b += 2;
    put16(body + b, hlen + 5); b += 2;
    put16(body + b, hlen + 3); b += 2;
    body[b++] = 0x00;                              /* name type: host_name */
    put16(body + b, hlen); b += 2;
    copy_bytes(body + b, host, hlen); b += hlen;

    /* supported_groups: just x25519, no fallback curves */
    put16(body + b, 0x000a); b += 2;
    put16(body + b, 4); b += 2;
    put16(body + b, 2); b += 2;
    put16(body + b, 0x001d); b += 2;

    /* signature_algorithms - we never verify a cert so this list only
     * exists to keep servers that check for it happy */
    {
        static const uint16_t algs[] = {0x0403,0x0804,0x0401,0x0503,0x0805,0x0501,0x0806,0x0601,0x0201};
        int na = (int)(sizeof(algs)/sizeof(algs[0]));
        put16(body + b, 0x000d); b += 2;
        put16(body + b, 2 + na*2); b += 2;
        put16(body + b, na*2); b += 2;
        for (int i = 0; i < na; i++) { put16(body + b, algs[i]); b += 2; }
    }

    /* supported_versions - advertise 1.3 only, no downgrade path */
    put16(body + b, 0x002b); b += 2;
    put16(body + b, 3); b += 2;
    body[b++] = 2;
    put16(body + b, 0x0304); b += 2;

    /* key_share - our X25519 ephemeral public key, generated in tls_connect */
    put16(body + b, 0x0033); b += 2;
    put16(body + b, 38); b += 2;
    put16(body + b, 36); b += 2;
    put16(body + b, 0x001d); b += 2;
    put16(body + b, 32); b += 2;
    copy_bytes(body + b, g_pub, 32); b += 32;

    put16(body + ext_len_pos, b - ext_start);

    /* handshake message wrapper: record header + type(1)/length(3) + body */
    out[0] = 0x16; out[1] = 0x03; out[2] = 0x01;   /* handshake record, legacy version 0x0301 */
    int rec_len_pos = 3;
    out[5] = 0x01;                                  /* ClientHello */
    out[6] = (uint8_t)(b >> 16); out[7] = (uint8_t)(b >> 8); out[8] = (uint8_t)b;
    copy_bytes(out + 9, body, b);
    int total_hs = 4 + b;
    put16(out + rec_len_pos, total_hs);

    /* the transcript hash only ever sees handshake messages, never record headers */
    sha256_update(&g_th, out + 5, total_hs);

    return 5 + total_hs;
}

/* ==========================================================================
 * ServerHello parsing
 * ========================================================================== */

static int parse_server_hello(const uint8_t *p, int len, uint8_t server_pub[32])
{
    /* p is a handshake message: type(1) length(3) body */
    if (len < 4 || p[0] != 0x02) return -1;
    int blen = (p[1] << 16) | (p[2] << 8) | p[3];
    const uint8_t *b = p + 4;
    if (blen + 4 > len) return -1;
    int i = 0;
    if (blen < 2 + 32 + 1) return -1;
    i += 2;                                  /* legacy_version, ignored */

    /* the server_random field doubles as a HelloRetryRequest marker when
     * it equals this fixed value from RFC 8446 4.1.3 - check before
     * trusting anything else in this message */
    static const uint8_t hrr_magic[32] = {
        0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
        0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };
    int is_hrr = 1;
    for (int k = 0; k < 32; k++) if (b[i+k] != hrr_magic[k]) { is_hrr = 0; break; }
    if (is_hrr) return -2;   /* we don't retry with a different group, just bail */
    i += 32;
    int sidlen = b[i++]; i += sidlen;        /* echoed session id, unused */
    i += 2;                                  /* cipher_suite - we only offered one */
    i += 1;                                  /* legacy compression method */
    if (i + 2 > blen) return -1;
    int extlen = (b[i] << 8) | b[i+1]; i += 2;
    int extend = i + extlen;
    int got_key = 0;
    while (i + 4 <= extend) {
        int et = (b[i] << 8) | b[i+1];
        int el = (b[i+2] << 8) | b[i+3];
        i += 4;
        if (i + el > extend) break;
        if (et == 0x0033) {                  /* key_share */
            int grp = (b[i] << 8) | b[i+1];
            int klen = (b[i+2] << 8) | b[i+3];
            if (grp == 0x001d && klen == 32) { copy_bytes(server_pub, b + i + 4, 32); got_key = 1; }
        }
        i += el;
    }
    return got_key ? 0 : -1;
}

/* ==========================================================================
 * handshake secret derivation + state machine
 * ========================================================================== */

static void compute_handshake_secrets(const uint8_t server_pub[32])
{
    uint8_t shared[32], early[32], derived[32], empty_hash[32], th[32];
    x25519(shared, g_priv, server_pub);

    /* early_secret with no PSK, then the "derived" step down to the
     * handshake secret - this is the fixed key schedule ladder from
     * RFC 8446 7.1, not something we get to skip steps in */
    uint8_t zero[32]; fill_bytes(zero, 0, 32);
    hkdf_extract(0, 0, zero, 32, early);
    sha256("", 0, empty_hash);
    derive_secret(early, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, shared, 32, g_hs_secret);

    th_snapshot(th);                          /* transcript so far: ClientHello..ServerHello */
    derive_secret(g_hs_secret, "c hs traffic", th, g_c_hs);
    derive_secret(g_hs_secret, "s hs traffic", th, g_s_hs);
    derive_keyiv(g_c_hs, g_c_hs_key, g_c_hs_iv);
    derive_keyiv(g_s_hs, g_s_hs_key, g_s_hs_iv);
    g_c_hs_seq = g_s_hs_seq = 0;
}

static void compute_app_secrets(void)
{
    uint8_t derived[32], empty_hash[32], master[32], zero[32], th[32];
    fill_bytes(zero, 0, 32);
    sha256("", 0, empty_hash);
    derive_secret(g_hs_secret, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, zero, 32, master);

    th_snapshot(th);                          /* transcript: ClientHello..server Finished */
    derive_secret(master, "c ap traffic", th, g_c_ap);
    derive_secret(master, "s ap traffic", th, g_s_ap);
    derive_keyiv(g_c_ap, g_c_ap_key, g_c_ap_iv);
    derive_keyiv(g_s_ap, g_s_ap_key, g_s_ap_iv);
    g_c_ap_seq = g_s_ap_seq = 0;
}

/* Consumes as many complete handshake messages as are sitting in `hs` and
 * folds them into the transcript hash, verifying the server's Finished
 * once it shows up. Leftover partial message bytes get shifted to the
 * front of the buffer for next time. Returns 1 once Finished has been
 * seen and verified, 0 if we just need more data, <0 on a bad MAC. */
static int g_hs_done;

static int feed_handshake(uint8_t *hs, int *hs_len)
{
    int consumed = 0;
    while (*hs_len - consumed >= 4) {
        uint8_t *m = hs + consumed;
        int mtype = m[0];
        int mlen = (m[1] << 16) | (m[2] << 8) | m[3];
        if (4 + mlen > *hs_len - consumed) break;      /* message not fully buffered yet */

        if (mtype == 20) {                              /* Finished */
            uint8_t fkey[32], expect[32], th[32];
            th_snapshot(th);                            /* covers up to CertificateVerify, not this message */
            hkdf_expand_label(g_s_hs, "finished", 0, 0, fkey, 32);
            hmac_sha256(fkey, 32, th, 32, expect);
            int ok = (mlen == 32);
            if (ok) for (int i = 0; i < 32; i++) if (expect[i] != m[4 + i]) { ok = 0; break; }
            sha256_update(&g_th, m, 4 + mlen);          /* Finished itself joins the transcript regardless */
            if (!ok) { set_status("server Finished mismatch"); return -1; }
            g_hs_done = 1;
            consumed += 4 + mlen;
            *hs_len -= consumed;
            for (int i = 0; i < *hs_len; i++) hs[i] = hs[consumed + i];
            return 1;
        }

        sha256_update(&g_th, m, 4 + mlen);              /* EncryptedExtensions / Certificate / CertificateVerify */
        consumed += 4 + mlen;
    }
    *hs_len -= consumed;
    for (int i = 0; i < *hs_len; i++) hs[i] = hs[consumed + i];
    return 0;
}

/* TLS 1.3 doesn't use change_cipher_spec, but middleboxes that only know
 * TLS 1.2 choke if they never see one, so both sides send a fake one. */
static void send_dummy_ccs(void)
{
    uint8_t ccs[6] = {0x14, 0x03, 0x03, 0x00, 0x01, 0x01};
    tcp_send(ccs, 6);
}

int tls_connect(const char *host, uint32_t ip, uint16_t port)
{
    g_open = 0; g_hs_done = 0;
    g_in_off = g_in_len = 0;
    g_app_off = g_app_len = 0;
    sha256_init(&g_th);
    set_status("connecting");

    /* fresh ephemeral keypair + randoms for this connection */
    rng_bytes(g_priv, 32);
    g_priv[0] &= 248; g_priv[31] &= 127; g_priv[31] |= 64;
    x25519_base(g_pub, g_priv);
    rng_bytes(g_crandom, 32);
    rng_bytes(g_session, 32);

    if (tcp_connect(ip, port) != 0) { set_status("tcp connect failed"); return -1; }

    static uint8_t ch[1024];
    int chlen = build_client_hello(ch, host);
    if (tcp_send(ch, chlen) < 0) { set_status("clienthello send failed"); return -2; }

    /* ServerHello still arrives in the clear - only after this do we have
     * a shared secret to switch encryption on */
    uint8_t server_pub[32];
    int have_sh = 0;
    for (int guard = 0; guard < 8 && !have_sh; guard++) {
        if (read_record(6000)) { set_status("no ServerHello"); return -3; }
        if (g_rec_type == 0x14) continue;             /* the middlebox-friendly CCS, skip it */
        if (g_rec_type == 0x16) {
            int r = parse_server_hello(g_rec_payload, g_rec_len, server_pub);
            if (r == -2) { set_status("HelloRetryRequest unsupported"); return -4; }
            if (r != 0) { set_status("bad ServerHello"); return -5; }
            sha256_update(&g_th, g_rec_payload, g_rec_len);
            have_sh = 1;
        } else if (g_rec_type == 0x15) { set_status("server alert"); return -6; }
        else return -7;
    }

    compute_handshake_secrets(server_pub);

    /* everything from here on arrives as encrypted "application_data"
     * records that actually carry handshake messages inside */
    static uint8_t hs[HS_CAP];
    int hs_len = 0;
    set_status("handshake");
    for (int guard = 0; guard < 64 && !g_hs_done; guard++) {
        if (read_record(6000)) { set_status("handshake read failed"); return -8; }
        if (g_rec_type == 0x14) continue;
        if (g_rec_type != 0x17) { set_status("unexpected record"); return -9; }
        int it = decrypt_record(g_s_hs_key, g_s_hs_iv, &g_s_hs_seq);
        if (it < 0) { set_status("hs decrypt failed"); return -10; }
        if (it == 21) { set_status("encrypted alert"); return -11; }
        if (it != 22) continue;                       /* not a handshake message, drop it */
        if (hs_len + g_dec_len > HS_CAP) { set_status("hs buffer overflow"); return -12; }
        copy_bytes(hs + hs_len, g_dec, g_dec_len); hs_len += g_dec_len;
        int r = feed_handshake(hs, &hs_len);
        if (r < 0) return -13;
    }
    if (!g_hs_done) { set_status("handshake incomplete"); return -14; }

    /* our turn: dummy CCS, then our own Finished, HMAC'd over the
     * transcript up through the server's Finished */
    send_dummy_ccs();
    {
        uint8_t fkey[32], th[32], finished_msg[36];
        th_snapshot(th);
        hkdf_expand_label(g_c_hs, "finished", 0, 0, fkey, 32);
        finished_msg[0] = 20; finished_msg[1] = 0; finished_msg[2] = 0; finished_msg[3] = 32;
        hmac_sha256(fkey, 32, th, 32, finished_msg + 4);
        if (send_encrypted(22, finished_msg, 36, g_c_hs_key, g_c_hs_iv, &g_c_hs_seq) != 0) {
            set_status("client Finished send failed"); return -15;
        }
    }

    compute_app_secrets();
    g_open = 1;
    set_status("connected");
    return 0;
}

/* ==========================================================================
 * application data
 * ========================================================================== */

int tls_send(const uint8_t *data, int len)
{
    if (!g_open) return -1;
    int off = 0;
    while (off < len) {
        int chunk = len - off;
        if (chunk > 16000) chunk = 16000;      /* stay well under DEC_CAP with room for framing */
        if (send_encrypted(23, data + off, chunk, g_c_ap_key, g_c_ap_iv, &g_c_ap_seq) != 0)
            return off > 0 ? off : -1;
        off += chunk;
    }
    return len;
}

int tls_recv(uint8_t *buf, int cap, int timeout_ms)
{
    /* drain anything already decrypted and waiting from a previous call
     * before touching the network again */
    if (g_app_len > g_app_off) {
        int n = g_app_len - g_app_off; if (n > cap) n = cap;
        copy_bytes(buf, g_appbuf + g_app_off, n);
        g_app_off += n;
        if (g_app_off >= g_app_len) g_app_off = g_app_len = 0;
        return n;
    }
    if (!g_open) return -1;

    uint32_t t0 = clock_ms();
    for (;;) {
        if (read_record(timeout_ms)) {
            if (tcp_closed()) { g_open = 0; return -1; }
            if ((uint32_t)(clock_ms() - t0) >= (uint32_t)timeout_ms) return 0;
            continue;
        }
        if (g_rec_type == 0x14) continue;
        if (g_rec_type != 0x17) { g_open = 0; return -1; }
        int it = decrypt_record(g_s_ap_key, g_s_ap_iv, &g_s_ap_seq);
        if (it < 0) { g_open = 0; return -1; }
        if (it == 23) {                               /* application data, what we came for */
            int n = g_dec_len; if (n > cap) n = cap;
            copy_bytes(buf, g_dec, n);
            if (g_dec_len > n) {                       /* caller's buffer was smaller than the record */
                int extra = g_dec_len - n;
                if (extra <= DEC_CAP) { copy_bytes(g_appbuf, g_dec + n, extra); g_app_off = 0; g_app_len = extra; }
            }
            return n;
        }
        if (it == 21) { g_open = 0; return -1; }      /* alert, almost certainly close_notify */
        if (it == 22) {                               /* post-handshake handshake message */
            if (g_dec_len >= 1 && g_dec[0] == 24) {   /* KeyUpdate: ratchet the server's traffic secret forward */
                uint8_t next_secret[32];
                hkdf_expand_label(g_s_ap, "traffic upd", 0, 0, next_secret, 32);
                copy_bytes(g_s_ap, next_secret, 32);
                derive_keyiv(g_s_ap, g_s_ap_key, g_s_ap_iv);
                g_s_ap_seq = 0;
            }
            /* NewSessionTicket and anything else here: nothing to act on */
            continue;
        }
    }
}

int tls_is_open(void) { return g_open; }

void tls_close(void)
{
    if (g_open) {
        uint8_t alert[2] = {1, 0};                    /* level=warning, description=close_notify */
        send_encrypted(21, alert, 2, g_c_ap_key, g_c_ap_iv, &g_c_ap_seq);
    }
    tcp_close();
    g_open = 0;
}
