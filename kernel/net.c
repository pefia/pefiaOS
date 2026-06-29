#include "net.h"
#include "netstack.h"
#include "nic.h"
#include "tls.h"
#include "clock.h"
#include "inflate.h"

/* ========================================================================== */
/* small string helpers                                                       */
/* ========================================================================== */

static int seq(const char *a, const char *b) { int i=0; while(a[i]&&b[i]){if(a[i]!=b[i])return 0;i++;} return a[i]==b[i]; }
static int slen(const char *s){int n=0;while(s[n])n++;return n;}
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static int sieq(const char *a, const char *b){ int i=0; while(a[i]&&b[i]){ if(lc(a[i])!=lc(b[i]))return 0; i++; } return lc(a[i])==lc(b[i]); }
static void scopy(char *d, const char *s, int cap){ int i=0; if(cap<=0)return; while(s&&s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }

static int starts_ci(const char *s, const char *p){ int i=0; while(p[i]){ if(lc(s[i])!=lc(p[i])) return 0; i++; } return 1; }

/* ========================================================================== */
/* state                                                                      */
/* ========================================================================== */

#define RAW_CAP  262144
#define DEC_CAP  786432    /* decompressed body buffer (gzip can expand a lot) */

static int      g_ready = 0;
static char     g_status[80];
static uint8_t  g_raw[RAW_CAP];
static uint8_t  g_dec[DEC_CAP];

static void status(const char *s){ scopy(g_status, s, sizeof(g_status)); }

int net_init(void)
{
    g_ready = 0;
    status("Network: starting");
    clock_init();

    if (!nic_init()) { status("Network: no supported NIC (need Intel e1000 or RTL8139)"); return 0; }
    if (!netstack_init()) { status("Network: config failed"); return 0; }

    char ip[16]; netstack_ip_str(netstack_local_ip(), ip);
    char buf[80]; int o=0;
    const char *nm = nic_name(); while(*nm && o<24){ buf[o++]=*nm++; }
    const char *pfx=" IP "; for(int i=0;pfx[i];i++) buf[o++]=pfx[i];
    int k=0; while(ip[k]&&o<78) buf[o++]=ip[k++]; buf[o]=0;
    status(buf);
    g_ready = 1;
    return 1;
}

int net_ready(void) { return g_ready; }
const char *net_status_text(void) { return g_status; }
uint32_t net_local_ip(void) { return netstack_local_ip(); }

/* ========================================================================== */
/* URL parsing                                                                */
/* ========================================================================== */

typedef struct { char scheme[8]; char host[128]; int port; char path[1536]; } Url;

static void parse_url(const char *url, Url *u)
{
    scopy(u->scheme, "https", sizeof(u->scheme));
    u->host[0] = 0; u->path[0] = 0; u->port = 0;

    const char *p = url;
    if (starts_ci(p, "http://"))  { scopy(u->scheme, "http", sizeof(u->scheme)); p += 7; }
    else if (starts_ci(p, "https://")) { scopy(u->scheme, "https", sizeof(u->scheme)); p += 8; }

    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < 127) u->host[i++] = *p++;
    u->host[i] = 0;
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') { port = port*10 + (*p - '0'); p++; }
        u->port = port;
    }
    if (*p == '/') scopy(u->path, p, sizeof(u->path));
    else scopy(u->path, "/", sizeof(u->path));

    if (u->port == 0) u->port = seq(u->scheme, "https") ? 443 : 80;
}

/* Resolve a (possibly relative) Location against the current URL. */
static void putc_s(char *out, int *o, int cap, char c) { if (*o < cap - 1) out[(*o)++] = c; }

static void resolve_location(const Url *base, const char *loc, char *out, int cap)
{
    if (starts_ci(loc, "http://") || starts_ci(loc, "https://")) { scopy(out, loc, cap); return; }
    int o = 0;
    for (const char *s = base->scheme; *s; s++) putc_s(out, &o, cap, *s);
    putc_s(out, &o, cap, ':'); putc_s(out, &o, cap, '/'); putc_s(out, &o, cap, '/');
    for (const char *h = base->host; *h; h++) putc_s(out, &o, cap, *h);
    if ((seq(base->scheme,"https")&&base->port!=443)||(seq(base->scheme,"http")&&base->port!=80)) {
        char tmp[8]; int t = base->port, pi = 0;
        if (t == 0) tmp[pi++]='0'; else while (t) { tmp[pi++]=(char)('0'+t%10); t/=10; }
        putc_s(out, &o, cap, ':');
        while (pi) putc_s(out, &o, cap, tmp[--pi]);
    }
    if (loc[0] != '/') putc_s(out, &o, cap, '/');
    for (const char *l = loc; *l; l++) putc_s(out, &o, cap, *l);
    out[o] = 0;
}

/* ========================================================================== */
/* HTTP response parsing                                                       */
/* ========================================================================== */

static int find_crlfcrlf(const uint8_t *b, int len)
{
    for (int i = 0; i + 3 < len; i++)
        if (b[i]=='\r'&&b[i+1]=='\n'&&b[i+2]=='\r'&&b[i+3]=='\n') return i;
    /* tolerate bare LFLF */
    for (int i = 0; i + 1 < len; i++)
        if (b[i]=='\n'&&b[i+1]=='\n') return i;
    return -1;
}

static int parse_status(const uint8_t *b, int len)
{
    int i = 0;
    while (i < len && b[i] != ' ') i++;
    while (i < len && b[i] == ' ') i++;
    int code = 0, d = 0;
    while (i < len && b[i] >= '0' && b[i] <= '9') { code = code*10 + (b[i]-'0'); i++; d++; }
    return d ? code : -1;
}

/* Find header value (case-insensitive) within the header block [b,b+hlen). */
static int find_header(const uint8_t *b, int hlen, const char *name, char *out, int cap)
{
    int nl = slen(name);
    int i = 0;
    /* skip status line */
    while (i < hlen && b[i] != '\n') i++;
    i++;
    while (i < hlen) {
        int match = 1;
        for (int j = 0; j < nl; j++) { if (i+j>=hlen || lc(b[i+j]) != lc(name[j])) { match = 0; break; } }
        if (match && i+nl < hlen && b[i+nl] == ':') {
            int k = i + nl + 1;
            while (k < hlen && (b[k]==' '||b[k]=='\t')) k++;
            int o = 0;
            while (k < hlen && b[k] != '\r' && b[k] != '\n' && o < cap-1) out[o++] = b[k++];
            out[o] = 0;
            return 1;
        }
        while (i < hlen && b[i] != '\n') i++;
        i++;
    }
    out[0] = 0;
    return 0;
}

/* De-chunk transfer-encoding: chunked, in place. Returns new length. */
static int dechunk(uint8_t *b, int len)
{
    int in = 0, out = 0;
    while (in < len) {
        int sz = 0, any = 0;
        while (in < len) {
            char c = (char)b[in];
            int dig;
            if (c>='0'&&c<='9') dig=c-'0';
            else if (c>='a'&&c<='f') dig=10+(c-'a');
            else if (c>='A'&&c<='F') dig=10+(c-'A');
            else break;
            sz = sz*16 + dig; any = 1; in++;
        }
        while (in < len && b[in] != '\n') in++;   /* skip to end of size line */
        in++;
        if (!any || sz == 0) break;
        if (in + sz > len) sz = len - in;
        for (int k = 0; k < sz; k++) b[out++] = b[in++];
        if (in < len && b[in]=='\r') in++;
        if (in < len && b[in]=='\n') in++;
    }
    return out;
}

/* ========================================================================== */
/* One HTTP(S) request                                                         */
/* ========================================================================== */

static int recv_all_limit(int is_tls, int body_cap)
{
    int total = 0;
    int hdr_end = -1;
    int body_start = 0;
    uint32_t idle0 = clock_ms();
    for (;;) {
        int space = RAW_CAP - 1 - total;
        if (space <= 0) break;
        int n = is_tls ? tls_recv(g_raw + total, space, 700)
                       : tcp_recv(g_raw + total, space, 700);
        if (n > 0) {
            total += n;
            idle0 = clock_ms();

            if (hdr_end < 0) {
                hdr_end = find_crlfcrlf(g_raw, total);
                if (hdr_end >= 0) body_start = hdr_end + 4;
            }
            if (body_cap > 0 && hdr_end >= 0 && total - body_start >= body_cap) break;
            continue;
        }
        if (is_tls) { if (!tls_is_open()) break; }
        else        { if (tcp_closed())   break; }
        if ((uint32_t)(clock_ms() - idle0) > 9000) break;
    }
    g_raw[total] = 0;
    return total;
}

static int http_once(const Url *u, NetResponse *resp, char *loc_out, int loc_cap, int body_cap)
{
    uint32_t ip;
    if (dns_resolve(u->host, &ip) != 0) { status("DNS lookup failed"); resp->status = -2; return -1; }

    int is_tls = seq(u->scheme, "https");
    resp->is_tls = is_tls;

    if (is_tls) {
        if (tls_connect(u->host, ip, (uint16_t)u->port) != 0) {
            char m[80]; scopy(m, "TLS: ", sizeof(m));
            const char *t = tls_status(); int o=5; while(t[o-5]&&o<79){m[o]=t[o-5];o++;} m[o]=0;
            status(m); resp->status = -3; return -1;
        }
    } else {
        if (tcp_connect(ip, (uint16_t)u->port) != 0) { status("TCP connect failed"); resp->status = -3; return -1; }
    }

    /* build request */
    char req[1800]; int o = 0;
    const char *m1 = "GET ";
    for (int i=0;m1[i];i++) req[o++]=m1[i];
    for (int i=0;u->path[i];i++) req[o++]=u->path[i];
    const char *m2 = " HTTP/1.1\r\nHost: ";
    for (int i=0;m2[i];i++) req[o++]=m2[i];
    for (int i=0;u->host[i];i++) req[o++]=u->host[i];
    const char *m3 = "\r\nUser-Agent: pefiaOS/1.0 (browser)\r\nAccept: text/html,image/png,image/jpeg,image/gif,*/*\r\nAccept-Encoding: gzip, identity\r\nConnection: close\r\n\r\n";
    for (int i=0;m3[i];i++) req[o++]=m3[i];

    int sent = is_tls ? tls_send((const uint8_t*)req, o) : tcp_send((const uint8_t*)req, o);
    if (sent < 0) { status("request send failed"); resp->status = -4; }

    int total = recv_all_limit(is_tls, body_cap);
    if (is_tls) tls_close(); else tcp_close();

    if (total <= 0) { status("no response"); resp->status = -5; return -1; }

    int hdr_end = find_crlfcrlf(g_raw, total);
    int body_start = (hdr_end >= 0) ? hdr_end + 4 : 0;
    int hlen = (hdr_end >= 0) ? hdr_end : total;

    resp->status = parse_status(g_raw, total);

    char te[64]; find_header(g_raw, hlen, "transfer-encoding", te, sizeof(te));
    find_header(g_raw, hlen, "content-type", resp->content_type, sizeof(resp->content_type));

    int body_len = total - body_start;
    uint8_t *body = g_raw + body_start;

    if (te[0] && (sieq(te, "chunked") || starts_ci(te, "chunked")))
        body_len = dechunk(body, body_len);

    /* Content-Encoding: gzip/deflate -> inflate into g_dec */
    char ce[32]; find_header(g_raw, hlen, "content-encoding", ce, sizeof(ce));
    if (ce[0] && (starts_ci(ce, "gzip") || starts_ci(ce, "deflate") || starts_ci(ce, "x-gzip"))) {
        int dl = auto_inflate(body, body_len, g_dec, DEC_CAP - 1);
        if (dl > 0) { g_dec[dl] = 0; body = g_dec; body_len = dl; }
    }

    body[body_len] = 0;
    resp->body = (const char *)body;
    resp->body_len = body_len;

    if (loc_out) { loc_out[0] = 0; find_header(g_raw, hlen, "location", loc_out, loc_cap); }
    status("OK");
    return 0;
}

int net_fetch(const char *url, NetResponse *resp)
{
    if (!g_ready) { status("Network offline"); resp->status = -1; resp->body=""; resp->body_len=0; resp->final_url[0]=0; return -1; }

    char cur[256]; scopy(cur, url, sizeof(cur));
    Url u;

    for (int hop = 0; hop < 6; hop++) {
        char locbuf[256] = {0};
        parse_url(cur, &u);
        int r = http_once(&u, resp, locbuf, sizeof(locbuf), 0);
        scopy(resp->final_url, cur, sizeof(resp->final_url));
        if (r < 0) return r;

        int s = resp->status;
        if ((s==301||s==302||s==303||s==307||s==308) && locbuf[0]) {
            char nxt[256];
            resolve_location(&u, locbuf, nxt, sizeof(nxt));
            scopy(cur, nxt, sizeof(cur));
            status("Following redirect");
            continue;
        }
        status("Loaded");
        return 0;
    }
    status("Too many redirects");
    return 0;
}

int net_fetch_limited(const char *url, int max_body_bytes, NetResponse *resp)
{
    if (!g_ready) { status("Network offline"); resp->status = -1; resp->body=""; resp->body_len=0; resp->final_url[0]=0; return -1; }

    char cur[256]; scopy(cur, url, sizeof(cur));
    Url u;
    int cap = max_body_bytes > 0 ? max_body_bytes : 0;

    for (int hop = 0; hop < 6; hop++) {
        char locbuf[256] = {0};
        parse_url(cur, &u);
        int r = http_once(&u, resp, locbuf, sizeof(locbuf), cap);
        scopy(resp->final_url, cur, sizeof(resp->final_url));
        if (r < 0) return r;

        int s = resp->status;
        if ((s==301||s==302||s==303||s==307||s==308) && locbuf[0]) {
            char nxt[256];
            resolve_location(&u, locbuf, nxt, sizeof(nxt));
            scopy(cur, nxt, sizeof(cur));
            status("Following redirect");
            continue;
        }
        status("Loaded");
        return 0;
    }
    status("Too many redirects");
    return 0;
}
