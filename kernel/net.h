/* kernel/net.h - high level networking + HTTP(S) fetch for the browser. */
#ifndef PEFIA_NET_H
#define PEFIA_NET_H

#include <stdint.h>

typedef struct {
    int   status;            /* HTTP status code, or <0 on transport error */
    int   is_tls;
    char  final_url[256];    /* URL actually fetched after redirects */
    char  content_type[80];
    const char *body;        /* points into net.c's internal buffer */
    int   body_len;
} NetResponse;

int         net_init(void);              /* bring up NIC + IP config; 1 if link up */
int         net_ready(void);
const char *net_status_text(void);
uint32_t    net_local_ip(void);

/* Fetch a URL (http or https), following redirects. Fills resp and returns 0,
 * or negative on a hard transport failure (resp->status carries detail). */
int net_fetch(const char *url, NetResponse *resp);

/* Lightweight bounded fetch for auxiliary resources (e.g., script/image probe).
 * Same redirect behavior as net_fetch. Body pointer is valid until next net call. */
int net_fetch_limited(const char *url, int max_body_bytes, NetResponse *resp);

#endif /* PEFIA_NET_H */
