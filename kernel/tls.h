/* kernel/tls.h
 * -----------------------------------------------------------------------------
 * A minimal TLS 1.3 client (TLS_AES_128_GCM_SHA256 + X25519) on top of the
 * pefiaOS TCP client. One connection at a time. Server certificates are NOT
 * verified — this gets a hobby OS onto HTTPS sites, it does not authenticate
 * them. Returns 0 on success throughout.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_TLS_H
#define PEFIA_TLS_H

#include <stdint.h>

/* Open a TCP connection to ip:port and run the TLS 1.3 handshake for `host`
 * (used as SNI). Returns 0 on success, negative on failure. */
int  tls_connect(const char *host, uint32_t ip, uint16_t port);

/* Send application data (e.g. an HTTP request). Returns bytes or <0. */
int  tls_send(const uint8_t *data, int len);

/* Receive decrypted application data. Returns >0 bytes, 0 on idle/timeout,
 * <0 on close/error. */
int  tls_recv(uint8_t *buf, int cap, int timeout_ms);

int  tls_is_open(void);
void tls_close(void);

/* Last human-readable status/error (for diagnostics in the browser). */
const char *tls_status(void);

#endif /* PEFIA_TLS_H */
