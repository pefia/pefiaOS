#ifndef PEFIA_NET_H
#define PEFIA_NET_H

#include <stdint.h>

typedef struct {
    int   status;            /* HTTP status code, or <0 if we never got one */
    int   is_tls;
    char  final_url[512];    /* where we ended up after following redirects */
    char  content_type[80];
    const char *body;        /* points into net.c's scratch buffer - copy it if you need it later */
    int   body_len;
} NetResponse;

int         net_init(void);              /* bring up the NIC + IP config; 1 if we got a link */
int         net_ready(void);
const char *net_status_text(void);
uint32_t    net_local_ip(void);

/* Fetch a URL (http or https), following redirects up to a small cap.
 * Returns 0 with resp filled in, or negative on a transport-level failure
 * (resp->status still carries whatever detail we have). */
int net_fetch(const char *url, NetResponse *resp);

/* Same thing, but stop reading the body once it hits max_body_bytes - handy
 * for probing an image or script without pulling the whole thing down.
 * Redirects are followed the same way as net_fetch. */
int net_fetch_limited(const char *url, int max_body_bytes, NetResponse *resp);

/* POST a form body (application/x-www-form-urlencoded, NUL-terminated).
 * Redirects follow browser convention: 301/302/303 continue as a GET,
 * 307/308 re-send the same body. */
int net_fetch_post(const char *url, const char *body, NetResponse *resp);

#endif
