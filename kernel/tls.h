/* kernel/tls.h
 *
 * A single-connection TLS 1.3 client, just enough to get the browser onto
 * HTTPS sites: TLS_AES_128_GCM_SHA256 with X25519 for key exchange. Only
 * one connection is tracked at a time (see the globals in tls.c) - there's
 * no session pooling or renegotiation support here.
 *
 * Server certificates are not verified. We authenticate nobody; this just
 * gets bytes flowing over an encrypted channel the way a browser would
 * expect. Don't ship anything security-sensitive over it.
 */
#ifndef PEFIA_TLS_H
#define PEFIA_TLS_H

#include <stdint.h>

/* Opens a TCP connection to ip:port and drives the full TLS 1.3 handshake,
 * sending `host` as the SNI extension. 0 on success, negative on failure. */
int  tls_connect(const char *host, uint32_t ip, uint16_t port);

/* Encrypts and ships application data (e.g. a raw HTTP request). Returns
 * bytes written, or <0 on failure. */
int  tls_send(const uint8_t *data, int len);

/* Pulls decrypted application data off the wire. >0 bytes read, 0 if
 * nothing arrived before timeout_ms, <0 if the connection died. */
int  tls_recv(uint8_t *buf, int cap, int timeout_ms);

int  tls_is_open(void);
void tls_close(void);

/* Last human-readable status/error, purely for surfacing something useful
 * in the browser UI when a connection fails. */
const char *tls_status(void);

#endif /* PEFIA_TLS_H */
