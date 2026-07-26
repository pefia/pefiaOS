#include "net.h"
#include "netstack.h"
#include "nic.h"
#include "tls.h"
#include "clock.h"
#include "inflate.h"

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int str_eq_ci(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (to_lower(a[i]) != to_lower(b[i])) return 0; i++; }
    return to_lower(a[i]) == to_lower(b[i]);
}

static void str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int starts_with_ci(const char *s, const char *prefix)
{
    int i = 0;
    while (prefix[i]) { if (to_lower(s[i]) != to_lower(prefix[i])) return 0; i++; }
    return 1;
}

#define RAW_CAP 655360
#define DEC_CAP 1048576   /* decompressed body scratch space - gzip can expand a lot */

static int     net_link_up = 0;
static char    net_status_msg[80];
static uint8_t raw_buf[RAW_CAP];
static uint8_t dec_buf[DEC_CAP];

static void set_status(const char *s) { str_copy(net_status_msg, s, sizeof(net_status_msg)); }

/* Just enough cookie support to keep a session alive across redirects.
 * Keyed by (host, name); Path/Expires/Domain attributes are ignored.
 * Pragmatic, not RFC 6265. */
#define COOKIE_MAX 24

typedef struct {
    char host[96];   /* stored with any leading "www." stripped */
    char name[64];
    char val[256];
    int  used;
} Cookie;

static Cookie cookie_jar[COOKIE_MAX];

static const char *strip_www(const char *host)
{
    return starts_with_ci(host, "www.") ? host + 4 : host;
}

/* A cookie stored for "example.com" also applies to "www.example.com":
 * exact match, or the request host ends with "." + the stored host. */
static int cookie_host_match(const char *req_host, const char *stored)
{
    if (str_eq_ci(req_host, stored)) return 1;
    int rl = str_len(req_host), sl = str_len(stored);
    if (rl <= sl || req_host[rl - sl - 1] != '.') return 0;
    for (int i = 0; i < sl; i++)
        if (to_lower(req_host[rl - sl + i]) != to_lower(stored[i])) return 0;
    return 1;
}

static void cookie_store(const char *req_host, const char *name, const char *val)
{
    const char *host = strip_www(req_host);

    int slot = -1;
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (cookie_jar[i].used && str_eq_ci(cookie_jar[i].host, host) &&
            str_eq(cookie_jar[i].name, name)) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < COOKIE_MAX; i++)
            if (!cookie_jar[i].used) { slot = i; break; }
    }
    if (slot < 0) slot = 0;   /* jar full - recycle slot 0 rather than get clever */

    str_copy(cookie_jar[slot].host, host, sizeof(cookie_jar[slot].host));
    str_copy(cookie_jar[slot].name, name, sizeof(cookie_jar[slot].name));
    str_copy(cookie_jar[slot].val,  val,  sizeof(cookie_jar[slot].val));
    cookie_jar[slot].used = 1;
}

/* Builds "Cookie: n1=v1; n2=v2\r\n" from every jar entry that applies to
 * this host; leaves out empty if nothing matches. Never overflows - an
 * entry that doesn't fit is simply dropped. */
static void cookies_for_host(const char *host, char *out, int cap)
{
    int o = 0, any = 0;
    for (int i = 0; i < COOKIE_MAX; i++) {
        if (!cookie_jar[i].used || !cookie_host_match(host, cookie_jar[i].host)) continue;
        const char *pre = any ? "; " : "Cookie: ";
        int need = str_len(pre) + str_len(cookie_jar[i].name) + 1 + str_len(cookie_jar[i].val);
        if (o + need > cap - 3) break;   /* keep room for the closing CRLF + NUL */
        for (int j = 0; pre[j]; j++) out[o++] = pre[j];
        for (int j = 0; cookie_jar[i].name[j]; j++) out[o++] = cookie_jar[i].name[j];
        out[o++] = '=';
        for (int j = 0; cookie_jar[i].val[j]; j++) out[o++] = cookie_jar[i].val[j];
        any = 1;
    }
    if (any) { out[o++] = '\r'; out[o++] = '\n'; }
    out[o] = '\0';
}

int net_init(void)
{
    net_link_up = 0;
    set_status("Network: starting");
    clock_init();

    if (!nic_init()) {
        set_status("Network: no supported NIC (need Intel e1000 or RTL8139)");
        return 0;
    }
    if (!netstack_init()) {
        set_status("Network: config failed");
        return 0;
    }

    char ip_str[16];
    netstack_ip_str(netstack_local_ip(), ip_str);

    char msg[80];
    int o = 0;
    const char *nic = nic_name();
    while (*nic && o < 24) msg[o++] = *nic++;
    const char *sep = " IP ";
    for (int i = 0; sep[i]; i++) msg[o++] = sep[i];
    int k = 0;
    while (ip_str[k] && o < 78) msg[o++] = ip_str[k++];
    msg[o] = '\0';
    set_status(msg);

    net_link_up = 1;
    return 1;
}

int net_ready(void) { return net_link_up; }
const char *net_status_text(void) { return net_status_msg; }
uint32_t net_local_ip(void) { return netstack_local_ip(); }

typedef struct {
    char scheme[8];
    char host[128];
    int  port;
    char path[1536];
} Url;

static void parse_url(const char *url, Url *u)
{
    str_copy(u->scheme, "https", sizeof(u->scheme));
    u->host[0] = '\0';
    u->path[0] = '\0';
    u->port = 0;

    const char *p = url;
    if (starts_with_ci(p, "http://"))       { str_copy(u->scheme, "http",  sizeof(u->scheme)); p += 7; }
    else if (starts_with_ci(p, "https://")) { str_copy(u->scheme, "https", sizeof(u->scheme)); p += 8; }

    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < 127) u->host[i++] = *p++;
    u->host[i] = '\0';

    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
        u->port = port;
    }

    if (*p == '/') str_copy(u->path, p, sizeof(u->path));
    else str_copy(u->path, "/", sizeof(u->path));

    if (u->port == 0) u->port = str_eq(u->scheme, "https") ? 443 : 80;
}

static void append_char(char *out, int *o, int cap, char c) { if (*o < cap - 1) out[(*o)++] = c; }

/* Resolves a (possibly relative) Location header against the URL that
 * produced it, so a redirect to "/login" doesn't lose the host. */
static void resolve_location(const Url *base, const char *loc, char *out, int cap)
{
    if (starts_with_ci(loc, "http://") || starts_with_ci(loc, "https://")) {
        str_copy(out, loc, cap);
        return;
    }

    int o = 0;
    for (const char *s = base->scheme; *s; s++) append_char(out, &o, cap, *s);
    append_char(out, &o, cap, ':'); append_char(out, &o, cap, '/'); append_char(out, &o, cap, '/');
    for (const char *h = base->host; *h; h++) append_char(out, &o, cap, *h);

    int non_default_port = (str_eq(base->scheme, "https") && base->port != 443) ||
                            (str_eq(base->scheme, "http")  && base->port != 80);
    if (non_default_port) {
        char digits[8];
        int v = base->port, n = 0;
        if (v == 0) digits[n++] = '0';
        else while (v) { digits[n++] = (char)('0' + v % 10); v /= 10; }
        append_char(out, &o, cap, ':');
        while (n) append_char(out, &o, cap, digits[--n]);
    }

    if (loc[0] != '/') append_char(out, &o, cap, '/');
    for (const char *l = loc; *l; l++) append_char(out, &o, cap, *l);
    out[o] = '\0';
}

static int find_header_end(const uint8_t *b, int len)
{
    for (int i = 0; i + 3 < len; i++)
        if (b[i] == '\r' && b[i+1] == '\n' && b[i+2] == '\r' && b[i+3] == '\n') return i;
    for (int i = 0; i + 1 < len; i++)
        if (b[i] == '\n' && b[i+1] == '\n') return i;
    return -1;
}

static int parse_status_line(const uint8_t *b, int len)
{
    int i = 0;
    while (i < len && b[i] != ' ') i++;
    while (i < len && b[i] == ' ') i++;
    int code = 0, digits = 0;
    while (i < len && b[i] >= '0' && b[i] <= '9') { code = code * 10 + (b[i] - '0'); i++; digits++; }
    return digits ? code : -1;
}

/* Like find_header below, but starts scanning at byte offset `from` and
 * returns the offset just past the matched line, so callers can walk
 * repeated headers (Set-Cookie, mostly). Returns -1 when no more match. */
static int find_header_from(const uint8_t *b, int hlen, const char *name, int from, char *out, int cap)
{
    int name_len = str_len(name);
    int i = from;
    if (i == 0) {
        while (i < hlen && b[i] != '\n') i++;
        i++;
    }

    while (i < hlen) {
        int matches = 1;
        for (int j = 0; j < name_len; j++) {
            if (i + j >= hlen || to_lower(b[i+j]) != to_lower(name[j])) { matches = 0; break; }
        }
        if (matches && i + name_len < hlen && b[i + name_len] == ':') {
            int k = i + name_len + 1;
            while (k < hlen && (b[k] == ' ' || b[k] == '\t')) k++;
            int o = 0;
            while (k < hlen && b[k] != '\r' && b[k] != '\n' && o < cap - 1) out[o++] = b[k++];
            out[o] = '\0';
            while (k < hlen && b[k] != '\n') k++;
            return k + 1;
        }
        while (i < hlen && b[i] != '\n') i++;
        i++;
    }
    out[0] = '\0';
    return -1;
}

static int find_header(const uint8_t *b, int hlen, const char *name, char *out, int cap)
{
    return find_header_from(b, hlen, name, 0, out, cap) >= 0;
}

/* Servers happily send several Set-Cookie headers per response and
 * find_header only ever sees the first - so walk them all. Only the
 * name=value part before the first ';' is kept; attributes are ignored. */
static void cookies_from_response(const uint8_t *b, int hlen, const char *req_host)
{
    char line[336];   /* enough for a max-size name=value pair */
    int off = 0;
    while ((off = find_header_from(b, hlen, "set-cookie", off, line, sizeof(line))) >= 0) {
        int end = 0;
        while (line[end] && line[end] != ';') end++;
        line[end] = '\0';
        int eq = 0;
        while (line[eq] && line[eq] != '=') eq++;
        if (eq == 0 || !line[eq]) continue;
        line[eq] = '\0';
        cookie_store(req_host, line, line + eq + 1);
    }
}

/* Un-chunks Transfer-Encoding: chunked in place; returns the decoded length. */
static int dechunk(uint8_t *b, int len)
{
    int in = 0, out = 0;
    while (in < len) {
        int size = 0, saw_digit = 0;
        while (in < len) {
            char c = (char)b[in];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
            else break;
            /* Clamp instead of accumulating: the size comes from the server,
             * and an unbounded "7FFFFFFF..." would overflow the int and turn
             * the bounds check below into a wild copy past the buffer. */
            if (size <= len) size = size * 16 + digit;
            saw_digit = 1;
            in++;
        }
        while (in < len && b[in] != '\n') in++;   /* skip rest of the size line */
        in++;
        if (!saw_digit || size <= 0 || in >= len) break;
        if (size > len - in) size = len - in;     /* len - in can't overflow: both are valid indices */
        for (int k = 0; k < size; k++) b[out++] = b[in++];
        if (in < len && b[in] == '\r') in++;
        if (in < len && b[in] == '\n') in++;
    }
    return out;
}

static int recv_until_done(int is_tls, int body_cap)
{
    int total = 0;
    int header_end = -1;
    int body_start = 0;
    uint32_t last_activity = clock_ms();

    for (;;) {
        int space = RAW_CAP - 1 - total;
        if (space <= 0) break;

        int n = is_tls ? tls_recv(raw_buf + total, space, 700)
                       : tcp_recv(raw_buf + total, space, 700);
        if (n > 0) {
            total += n;
            last_activity = clock_ms();

            if (header_end < 0) {
                header_end = find_header_end(raw_buf, total);
                if (header_end >= 0) body_start = header_end + 4;
            }
            if (body_cap > 0 && header_end >= 0 && total - body_start >= body_cap) break;
            continue;
        }

        if (is_tls) { if (!tls_is_open()) break; }
        else        { if (tcp_closed())   break; }
        if ((uint32_t)(clock_ms() - last_activity) > 9000) break;
    }

    raw_buf[total] = '\0';
    return total;
}

static int do_request(const Url *u, int is_post, const char *post_body, NetResponse *resp,
                      char *location_out, int location_cap, int body_cap)
{
    uint32_t ip;
    if (dns_resolve(u->host, &ip) != 0) {
        set_status("DNS lookup failed");
        resp->status = -2;
        return -1;
    }

    int is_tls = str_eq(u->scheme, "https");
    resp->is_tls = is_tls;

    if (is_tls) {
        if (tls_connect(u->host, ip, (uint16_t)u->port) != 0) {
            char msg[80];
            str_copy(msg, "TLS: ", sizeof(msg));
            const char *reason = tls_status();
            int o = 5;
            while (reason[o - 5] && o < 79) { msg[o] = reason[o - 5]; o++; }
            msg[o] = '\0';
            set_status(msg);
            resp->status = -3;
            return -1;
        }
    } else {
        if (tcp_connect(ip, (uint16_t)u->port) != 0) {
            set_status("TCP connect failed");
            resp->status = -3;
            return -1;
        }
    }

    /* Worst case fits: path 1535 + host 127 + fixed headers + cookies 1023
     * leaves over 1k of room for a POST body; anything beyond that gets
     * truncated below rather than overflowing. */
    char req[4096];
    char cookie_hdr[1024];
    cookies_for_host(u->host, cookie_hdr, sizeof(cookie_hdr));

    int o = 0;
    const char *line1 = is_post ? "POST " : "GET ";
    for (int i = 0; line1[i]; i++) req[o++] = line1[i];
    for (int i = 0; u->path[i]; i++) req[o++] = u->path[i];
    const char *line2 = " HTTP/1.1\r\nHost: ";
    for (int i = 0; line2[i]; i++) req[o++] = line2[i];
    for (int i = 0; u->host[i]; i++) req[o++] = u->host[i];
    const char *rest = "\r\nUser-Agent: Mozilla/5.0 (compatible; pefiaOS/1.0)\r\nAccept: text/html,image/png,image/jpeg,image/gif,*/*\r\nAccept-Language: en-US,en;q=0.8\r\nAccept-Encoding: gzip, identity\r\nConnection: close\r\n";
    for (int i = 0; rest[i]; i++) req[o++] = rest[i];
    for (int i = 0; cookie_hdr[i]; i++) req[o++] = cookie_hdr[i];

    if (is_post) {
        const char *ct = "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
        for (int i = 0; ct[i]; i++) req[o++] = ct[i];
        int blen = post_body ? str_len(post_body) : 0;
        int room = (int)sizeof(req) - o - 10;
        if (room < 0) room = 0;
        if (blen > room) blen = room;
        char digits[8];
        int v = blen, n = 0;
        if (v == 0) digits[n++] = '0';
        else while (v) { digits[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) req[o++] = digits[--n];
        req[o++] = '\r'; req[o++] = '\n'; req[o++] = '\r'; req[o++] = '\n';
        for (int i = 0; i < blen; i++) req[o++] = post_body[i];
    } else {
        req[o++] = '\r'; req[o++] = '\n';
    }

    int sent = is_tls ? tls_send((const uint8_t *)req, o) : tcp_send((const uint8_t *)req, o);
    if (sent < 0) {
        set_status("request send failed");
        resp->status = -4;
    }

    int total = recv_until_done(is_tls, body_cap);
    if (is_tls) tls_close(); else tcp_close();

    if (total <= 0) {
        set_status("no response");
        resp->status = -5;
        return -1;
    }

    int header_end = find_header_end(raw_buf, total);
    int body_start = (header_end >= 0) ? header_end + 4 : 0;
    int header_len = (header_end >= 0) ? header_end : total;

    resp->status = parse_status_line(raw_buf, total);

    cookies_from_response(raw_buf, header_len, u->host);

    char transfer_encoding[64];
    find_header(raw_buf, header_len, "transfer-encoding", transfer_encoding, sizeof(transfer_encoding));
    find_header(raw_buf, header_len, "content-type", resp->content_type, sizeof(resp->content_type));

    int body_len = total - body_start;
    uint8_t *body = raw_buf + body_start;

    if (transfer_encoding[0] && starts_with_ci(transfer_encoding, "chunked"))
        body_len = dechunk(body, body_len);

    char content_encoding[32];
    find_header(raw_buf, header_len, "content-encoding", content_encoding, sizeof(content_encoding));
    if (content_encoding[0] && (starts_with_ci(content_encoding, "gzip") ||
                                 starts_with_ci(content_encoding, "deflate") ||
                                 starts_with_ci(content_encoding, "x-gzip"))) {
        int decoded_len = auto_inflate(body, body_len, dec_buf, DEC_CAP - 1);
        if (decoded_len > 0) {
            dec_buf[decoded_len] = '\0';
            body = dec_buf;
            body_len = decoded_len;
        }
    }

    body[body_len] = '\0';
    resp->body = (const char *)body;
    resp->body_len = body_len;

    if (location_out) {
        location_out[0] = '\0';
        find_header(raw_buf, header_len, "location", location_out, location_cap);
    }
    set_status("OK");
    return 0;
}

/* Shared by net_fetch, net_fetch_limited and net_fetch_post - the only
 * differences are whether the body read is capped (cap = 0 means "no
 * limit") and whether we start out as a POST (post_body non-NULL). */
static int fetch_with_cap(const char *url, int cap, const char *post_body, NetResponse *resp)
{
    if (!net_link_up) {
        set_status("Network offline");
        resp->status = -1;
        resp->body = "";
        resp->body_len = 0;
        resp->final_url[0] = '\0';
        return -1;
    }

    char current[512];
    str_copy(current, url, sizeof(current));
    Url u;
    const char *body = post_body;   /* non-NULL means this hop is a POST */

    for (int hop = 0; hop < 6; hop++) {
        char location[512] = { 0 };
        parse_url(current, &u);
        int r = do_request(&u, body != 0, body, resp, location, sizeof(location), cap);
        str_copy(resp->final_url, current, sizeof(resp->final_url));
        if (r < 0) return r;

        int status = resp->status;
        int is_redirect = (status == 301 || status == 302 || status == 303 ||
                            status == 307 || status == 308);
        if (is_redirect && location[0]) {
            char next[512];
            resolve_location(&u, location, next, sizeof(next));
            str_copy(current, next, sizeof(current));
            if (body && status != 307 && status != 308)
                body = 0;   /* 301/302/303 downgrade the POST to a GET */
            set_status("Following redirect");
            continue;
        }
        set_status("Loaded");
        return 0;
    }
    set_status("Too many redirects");
    return 0;
}

int net_fetch(const char *url, NetResponse *resp)
{
    return fetch_with_cap(url, 0, 0, resp);
}

int net_fetch_limited(const char *url, int max_body_bytes, NetResponse *resp)
{
    return fetch_with_cap(url, max_body_bytes > 0 ? max_body_bytes : 0, 0, resp);
}

int net_fetch_post(const char *url, const char *body, NetResponse *resp)
{
    return fetch_with_cap(url, 0, body ? body : "", resp);
}
