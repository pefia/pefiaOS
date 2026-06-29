#include "netstack.h"
#include "nic.h"
#include "clock.h"

/* ========================================================================== */
/* Small helpers                                                              */
/* ========================================================================== */

static void *nmemcpy(void *d, const void *s, int n)
{
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    for (int i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
static void nmemset(void *d, int c, int n)
{
    uint8_t *dd = (uint8_t *)d;
    for (int i = 0; i < n; i++) dd[i] = (uint8_t)c;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void ip_to_bytes(uint32_t ip, uint8_t out[4])
{ out[0] = (uint8_t)(ip >> 24); out[1] = (uint8_t)(ip >> 16); out[2] = (uint8_t)(ip >> 8); out[3] = (uint8_t)ip; }
static uint32_t ip_from_bytes(const uint8_t b[4])
{ return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }

/* RFC1071 internet checksum over a byte range, with a carry-in accumulator. */
static uint32_t csum_acc(uint32_t sum, const uint8_t *data, int len)
{
    int i = 0;
    for (; i + 1 < len; i += 2) sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (i < len) sum += (uint32_t)(data[i] << 8);
    return sum;
}
static uint16_t csum_fin(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

/* ========================================================================== */
/* Stack state                                                                */
/* ========================================================================== */

#define ETH_HDR   14
#define IP_HDR    20

#define ET_IPV4   0x0800
#define ET_ARP    0x0806
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

static uint8_t  g_mac[6];
static uint8_t  g_bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint32_t g_ip   = 0;
static uint32_t g_gw   = 0;
static uint32_t g_mask = 0;
static uint32_t g_dns  = 0;
static int      g_up   = 0;

static uint8_t  g_tx[2048];

/* ---- ARP cache ---- */
typedef struct { uint32_t ip; uint8_t mac[6]; int valid; } ArpEntry;
#define ARP_CACHE 8
static ArpEntry g_arp[ARP_CACHE];

/* ---- captured UDP datagram (DHCP/DNS) ---- */
static uint8_t  g_udp_buf[1500];
static int      g_udp_len = 0;
static int      g_udp_ready = 0;
static uint16_t g_udp_filter = 0;    /* host-order dst port we want */

/* ---- TCP single connection ---- */
enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTAB, TCP_CLOSE_WAIT };
#define RCV_CAP 49152
static struct {
    int      state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    uint8_t  remote_mac[6];
    uint32_t snd_una, snd_nxt;       /* send sequence space */
    uint32_t rcv_nxt;                /* next expected seq */
    int      syn_acked;
    int      fin_recv;
    int      reset;
    uint8_t  rbuf[RCV_CAP];
    int      rhead, rtail;           /* ring: data in [rtail,rhead) */
} g_tcp;

static int rcv_used(void) { int d = g_tcp.rhead - g_tcp.rtail; if (d < 0) d += RCV_CAP; return d; }
static int rcv_free(void) { return RCV_CAP - 1 - rcv_used(); }

/* ========================================================================== */
/* Ethernet / ARP                                                             */
/* ========================================================================== */

static void eth_send(const uint8_t dst[6], uint16_t ethertype, const uint8_t *payload, int len)
{
    nmemcpy(g_tx, dst, 6);
    nmemcpy(g_tx + 6, g_mac, 6);
    wr16(g_tx + 12, ethertype);
    if (payload && len > 0) nmemcpy(g_tx + ETH_HDR, payload, len);
    nic_send(g_tx, (uint16_t)(ETH_HDR + len));
}

static void arp_learn(uint32_t ip, const uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { nmemcpy(g_arp[i].mac, mac, 6); return; }
    for (int i = 0; i < ARP_CACHE; i++)
        if (!g_arp[i].valid) { g_arp[i].ip = ip; nmemcpy(g_arp[i].mac, mac, 6); g_arp[i].valid = 1; return; }
    g_arp[0].ip = ip; nmemcpy(g_arp[0].mac, mac, 6); g_arp[0].valid = 1;
}
static int arp_lookup(uint32_t ip, uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { nmemcpy(mac, g_arp[i].mac, 6); return 1; }
    return 0;
}

static void arp_send(int reply, uint32_t target_ip, const uint8_t target_mac[6])
{
    uint8_t p[28];
    wr16(p + 0, 1);            /* HTYPE ethernet */
    wr16(p + 2, ET_IPV4);     /* PTYPE */
    p[4] = 6; p[5] = 4;
    wr16(p + 6, reply ? 2 : 1);
    nmemcpy(p + 8, g_mac, 6);
    ip_to_bytes(g_ip, p + 14);
    if (reply) nmemcpy(p + 18, target_mac, 6); else nmemset(p + 18, 0, 6);
    ip_to_bytes(target_ip, p + 24);
    eth_send(reply ? target_mac : g_bcast, ET_ARP, p, 28);
}

static void handle_arp(const uint8_t *f, int len)
{
    if (len < ETH_HDR + 28) return;
    const uint8_t *a = f + ETH_HDR;
    uint16_t op = rd16(a + 6);
    uint32_t sender_ip = ip_from_bytes(a + 14);
    uint32_t target_ip = ip_from_bytes(a + 24);
    arp_learn(sender_ip, a + 8);
    if (op == 1 && target_ip == g_ip)         /* request for us -> reply */
        arp_send(1, sender_ip, a + 8);
}

/* Resolve the MAC for the next hop toward `ip`. Blocks (polls) up to timeout. */
static int resolve_mac(uint32_t ip, uint8_t mac[6])
{
    uint32_t nexthop = ((ip & g_mask) == (g_ip & g_mask)) ? ip : g_gw;
    if (arp_lookup(nexthop, mac)) return 1;

    for (int tries = 0; tries < 6; tries++) {
        arp_send(0, nexthop, 0);
        uint32_t t0 = clock_ms();
        while ((uint32_t)(clock_ms() - t0) < 300) {
            netstack_poll();
            if (arp_lookup(nexthop, mac)) return 1;
        }
    }
    return 0;
}

/* ========================================================================== */
/* IPv4                                                                       */
/* ========================================================================== */

static uint16_t g_ip_id = 0x1000;

static int ip_send(uint8_t proto, uint32_t dst, const uint8_t *payload, int plen)
{
    uint8_t mac[6];
    if (dst == 0xFFFFFFFFu) {
        for (int i = 0; i < 6; i++) mac[i] = 0xFF;     /* broadcast: no ARP */
    } else if (!resolve_mac(dst, mac)) {
        return -1;
    }

    uint8_t hdr[IP_HDR];
    hdr[0] = 0x45;                 /* v4, ihl 5 */
    hdr[1] = 0x00;                 /* DSCP/ECN */
    wr16(hdr + 2, (uint16_t)(IP_HDR + plen));
    wr16(hdr + 4, g_ip_id++);
    wr16(hdr + 6, 0x4000);         /* don't fragment */
    hdr[8] = 64;                   /* TTL */
    hdr[9] = proto;
    wr16(hdr + 10, 0);             /* checksum placeholder */
    ip_to_bytes(g_ip, hdr + 12);
    ip_to_bytes(dst, hdr + 16);
    wr16(hdr + 10, csum_fin(csum_acc(0, hdr, IP_HDR)));

    /* Assemble the full IP datagram into the ethernet payload region. */
    static uint8_t buf[1600];
    nmemcpy(buf, hdr, IP_HDR);
    if (payload && plen > 0) nmemcpy(buf + IP_HDR, payload, plen);
    eth_send(mac, ET_IPV4, buf, IP_HDR + plen);
    return 0;
}

/* ========================================================================== */
/* ICMP echo reply (so the host can ping us)                                  */
/* ========================================================================== */

static void handle_icmp(uint32_t src, const uint8_t *p, int len)
{
    if (len < 8) return;
    if (p[0] != 8) return;                       /* echo request only */
    static uint8_t r[1500];
    if (len > (int)sizeof(r)) return;
    nmemcpy(r, p, len);
    r[0] = 0;                                    /* echo reply */
    wr16(r + 2, 0);
    wr16(r + 2, csum_fin(csum_acc(0, r, len)));
    ip_send(IPPROTO_ICMP, src, r, len);
}

/* ========================================================================== */
/* UDP                                                                        */
/* ========================================================================== */

static void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                     const uint8_t *data, int len)
{
    static uint8_t seg[1500];
    int total = 8 + len;
    wr16(seg + 0, src_port);
    wr16(seg + 2, dst_port);
    wr16(seg + 4, (uint16_t)total);
    wr16(seg + 6, 0);
    if (data && len > 0) nmemcpy(seg + 8, data, len);

    /* UDP checksum with IPv4 pseudo-header (optional, but compute it). */
    uint8_t ph[12];
    ip_to_bytes(g_ip, ph + 0);
    ip_to_bytes(dst_ip, ph + 4);
    ph[8] = 0; ph[9] = IPPROTO_UDP; wr16(ph + 10, (uint16_t)total);
    uint32_t s = csum_acc(0, ph, 12);
    s = csum_acc(s, seg, total);
    uint16_t ck = csum_fin(s);
    if (ck == 0) ck = 0xFFFF;
    wr16(seg + 6, ck);

    ip_send(IPPROTO_UDP, dst_ip, seg, total);
}

static void handle_udp(uint32_t src, const uint8_t *p, int len)
{
    (void)src;
    if (len < 8) return;
    uint16_t dport = rd16(p + 2);
    int dlen = (int)rd16(p + 4) - 8;
    if (dlen < 0 || dlen > len - 8) dlen = len - 8;
    if (dport == g_udp_filter && dlen <= (int)sizeof(g_udp_buf)) {
        nmemcpy(g_udp_buf, p + 8, dlen);
        g_udp_len = dlen;
        g_udp_ready = 1;
    }
}

/* Block until a UDP datagram arrives on the filtered port, or timeout. */
static int udp_wait(int timeout_ms)
{
    uint32_t t0 = clock_ms();
    while ((uint32_t)(clock_ms() - t0) < (uint32_t)timeout_ms) {
        netstack_poll();
        if (g_udp_ready) return 1;
    }
    return 0;
}

/* ========================================================================== */
/* TCP                                                                        */
/* ========================================================================== */

static void tcp_xmit(uint8_t flags, const uint8_t *data, int dlen)
{
    static uint8_t seg[1600];
    int hlen = 20;
    if (flags & 0x02) {                  /* SYN: advertise MSS 1460 */
        seg[20] = 2; seg[21] = 4; wr16(seg + 22, 1460);
        hlen = 24;
    }
    wr16(seg + 0, g_tcp.local_port);
    wr16(seg + 2, g_tcp.remote_port);
    wr32(seg + 4, g_tcp.snd_nxt);
    wr32(seg + 8, g_tcp.rcv_nxt);
    seg[12] = (uint8_t)((hlen / 4) << 4);
    seg[13] = flags;
    int win = rcv_free(); if (win > 65535) win = 65535;
    wr16(seg + 14, (uint16_t)win);
    wr16(seg + 16, 0);                   /* checksum */
    wr16(seg + 18, 0);                   /* urgent */
    if (data && dlen > 0) nmemcpy(seg + hlen, data, dlen);

    int total = hlen + dlen;

    uint8_t ph[12];
    ip_to_bytes(g_tcp.local_ip, ph + 0);
    ip_to_bytes(g_tcp.remote_ip, ph + 4);
    ph[8] = 0; ph[9] = IPPROTO_TCP; wr16(ph + 10, (uint16_t)total);
    uint32_t s = csum_acc(0, ph, 12);
    s = csum_acc(s, seg, total);
    wr16(seg + 16, csum_fin(s));

    /* TCP goes straight out via ip_send (remote MAC already resolved). */
    ip_send(IPPROTO_TCP, g_tcp.remote_ip, seg, total);
}

static void tcp_ack(void) { tcp_xmit(0x10, 0, 0); }

static void handle_tcp(uint32_t src, const uint8_t *p, int len)
{
    if (len < 20) return;
    uint16_t sport = rd16(p + 0), dport = rd16(p + 2);
    if (g_tcp.state == TCP_CLOSED) return;
    if (src != g_tcp.remote_ip || sport != g_tcp.remote_port || dport != g_tcp.local_port) return;

    uint32_t seq = rd32(p + 4);
    uint32_t ack = rd32(p + 8);
    uint8_t  off = (uint8_t)((p[12] >> 4) * 4);
    uint8_t  flags = p[13];
    const uint8_t *data = p + off;
    int dlen = len - off;
    if (dlen < 0) dlen = 0;

    if (flags & 0x04) { g_tcp.reset = 1; g_tcp.state = TCP_CLOSED; return; } /* RST */

    if (g_tcp.state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12) {           /* SYN+ACK */
            g_tcp.rcv_nxt = seq + 1;
            g_tcp.snd_una = ack;
            g_tcp.state = TCP_ESTAB;
            g_tcp.syn_acked = 1;
            tcp_ack();
        }
        return;
    }

    if (flags & 0x10) {                          /* ACK advances snd_una */
        if (ack - g_tcp.snd_una <= (g_tcp.snd_nxt - g_tcp.snd_una))
            g_tcp.snd_una = ack;
    }

    /* In-order data only (good enough over NAT/loopback paths). */
    if (dlen > 0 && seq == g_tcp.rcv_nxt) {
        int n = dlen; if (n > rcv_free()) n = rcv_free();
        for (int i = 0; i < n; i++) {
            g_tcp.rbuf[g_tcp.rhead] = data[i];
            g_tcp.rhead = (g_tcp.rhead + 1) % RCV_CAP;
        }
        g_tcp.rcv_nxt += n;
        tcp_ack();
    } else if (dlen > 0 && seq != g_tcp.rcv_nxt) {
        tcp_ack();                               /* duplicate / out of order */
    }

    if (flags & 0x01) {                          /* FIN */
        if (seq + (uint32_t)dlen == g_tcp.rcv_nxt) {
            g_tcp.rcv_nxt += 1;
            g_tcp.fin_recv = 1;
            g_tcp.state = TCP_CLOSE_WAIT;
            tcp_ack();
        }
    }
}

int tcp_connect(uint32_t ip, uint16_t port)
{
    nmemset(&g_tcp, 0, sizeof(g_tcp));
    g_tcp.local_ip = g_ip;
    g_tcp.remote_ip = ip;
    g_tcp.remote_port = port;
    g_tcp.local_port = (uint16_t)(49152 + (rdtsc() & 0x3FFF));
    uint32_t isn = (uint32_t)rdtsc();
    g_tcp.snd_una = isn;
    g_tcp.snd_nxt = isn;
    g_tcp.rcv_nxt = 0;
    g_tcp.state = TCP_SYN_SENT;

    if (!resolve_mac(ip, g_tcp.remote_mac)) { g_tcp.state = TCP_CLOSED; return -1; }

    for (int tries = 0; tries < 6; tries++) {
        g_tcp.snd_nxt = isn;                     /* SYN occupies seq isn */
        tcp_xmit(0x02, 0, 0);
        g_tcp.snd_nxt = isn + 1;
        uint32_t t0 = clock_ms();
        while ((uint32_t)(clock_ms() - t0) < 600) {
            netstack_poll();
            if (g_tcp.syn_acked) return 0;
            if (g_tcp.reset) return -2;
        }
    }
    g_tcp.state = TCP_CLOSED;
    return -3;
}

int tcp_send(const uint8_t *data, int len)
{
    if (g_tcp.state != TCP_ESTAB && g_tcp.state != TCP_CLOSE_WAIT) return -1;
    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1460) chunk = 1460;
        uint32_t seg_seq = g_tcp.snd_nxt;

        int acked = 0;
        for (int tries = 0; tries < 8 && !acked; tries++) {
            g_tcp.snd_nxt = seg_seq;
            tcp_xmit(0x18, data + sent, chunk);  /* PSH|ACK */
            g_tcp.snd_nxt = seg_seq + chunk;
            uint32_t t0 = clock_ms();
            while ((uint32_t)(clock_ms() - t0) < 500) {
                netstack_poll();
                if ((g_tcp.snd_una - seg_seq) >= (uint32_t)chunk &&
                    (g_tcp.snd_una - seg_seq) <= (uint32_t)len) { acked = 1; break; }
                if (g_tcp.reset) return -2;
            }
        }
        if (!acked) return sent > 0 ? sent : -3;
        sent += chunk;
    }
    return sent;
}

int tcp_recv(uint8_t *buf, int cap, int timeout_ms)
{
    uint32_t t0 = clock_ms();
    for (;;) {
        int avail = rcv_used();
        if (avail > 0) {
            int n = avail < cap ? avail : cap;
            for (int i = 0; i < n; i++) {
                buf[i] = g_tcp.rbuf[g_tcp.rtail];
                g_tcp.rtail = (g_tcp.rtail + 1) % RCV_CAP;
            }
            return n;
        }
        if (g_tcp.reset) return 0;
        if (g_tcp.fin_recv) return 0;
        if ((uint32_t)(clock_ms() - t0) >= (uint32_t)timeout_ms) return 0;
        netstack_poll();
    }
}

int tcp_closed(void)
{
    return (g_tcp.state == TCP_CLOSED || g_tcp.fin_recv || g_tcp.reset) && rcv_used() == 0;
}

void tcp_close(void)
{
    if (g_tcp.state == TCP_ESTAB || g_tcp.state == TCP_CLOSE_WAIT) {
        tcp_xmit(0x11, 0, 0);                    /* FIN|ACK */
        g_tcp.snd_nxt += 1;
        uint32_t t0 = clock_ms();
        while ((uint32_t)(clock_ms() - t0) < 300) netstack_poll();
    }
    g_tcp.state = TCP_CLOSED;
}

/* ========================================================================== */
/* RX dispatch                                                                */
/* ========================================================================== */

static void net_rx(const uint8_t *f, uint16_t len)
{
    if (len < ETH_HDR) return;
    uint16_t et = rd16(f + 12);

    if (et == ET_ARP) { handle_arp(f, len); return; }
    if (et != ET_IPV4) return;
    if (len < ETH_HDR + IP_HDR) return;

    const uint8_t *ip = f + ETH_HDR;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20) return;
    uint8_t proto = ip[9];
    uint32_t src = ip_from_bytes(ip + 12);
    uint32_t dst = ip_from_bytes(ip + 16);
    if (dst != g_ip && dst != 0xFFFFFFFFu) {
        /* still accept broadcast for DHCP before we own an address */
        if (!(g_ip == 0)) return;
    }
    int total = rd16(ip + 2);
    int plen = total - ihl;
    if (plen < 0 || plen > len - ETH_HDR - ihl) plen = len - ETH_HDR - ihl;
    const uint8_t *payload = ip + ihl;

    if (proto == IPPROTO_ICMP) handle_icmp(src, payload, plen);
    else if (proto == IPPROTO_UDP) handle_udp(src, payload, plen);
    else if (proto == IPPROTO_TCP) handle_tcp(src, payload, plen);
}

void netstack_poll(void) { nic_poll(); }

/* ========================================================================== */
/* DHCP                                                                       */
/* ========================================================================== */

static uint32_t g_xid = 0x50454649;   /* "PEFI" */

static int dhcp_build(uint8_t *b, int msgtype, uint32_t req_ip, uint32_t server_id)
{
    nmemset(b, 0, 300);
    b[0] = 1; b[1] = 1; b[2] = 6; b[3] = 0;       /* op/htype/hlen/hops */
    wr32(b + 4, g_xid);
    wr16(b + 10, 0x8000);                          /* broadcast flag */
    nmemcpy(b + 28, g_mac, 6);                     /* chaddr */
    wr32(b + 236, 0x63825363);                     /* magic cookie */
    int o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = (uint8_t)msgtype;
    b[o++] = 55; b[o++] = 4; b[o++] = 1; b[o++] = 3; b[o++] = 6; b[o++] = 15;
    if (req_ip)    { b[o++] = 50; b[o++] = 4; ip_to_bytes(req_ip, b + o); o += 4; }
    if (server_id) { b[o++] = 54; b[o++] = 4; ip_to_bytes(server_id, b + o); o += 4; }
    b[o++] = 61; b[o++] = 7; b[o++] = 1; nmemcpy(b + o, g_mac, 6); o += 6;
    b[o++] = 255;
    return o < 300 ? 300 : o;                       /* min BOOTP body */
}

static int dhcp_parse(const uint8_t *b, int len, int *msgtype, uint32_t *yip,
                      uint32_t *sid, uint32_t *mask, uint32_t *gw, uint32_t *dns)
{
    if (len < 240 || rd32(b + 236) != 0x63825363) return 0;
    *yip = ip_from_bytes(b + 16);
    *msgtype = 0; *sid = 0; *mask = 0; *gw = 0; *dns = 0;
    int o = 240;
    while (o < len) {
        uint8_t opt = b[o++];
        if (opt == 255) break;
        if (opt == 0) continue;
        if (o >= len) break;
        uint8_t l = b[o++];
        if (o + l > len) break;
        if (opt == 53 && l == 1) *msgtype = b[o];
        else if (opt == 54 && l == 4) *sid = ip_from_bytes(b + o);
        else if (opt == 1 && l == 4) *mask = ip_from_bytes(b + o);
        else if (opt == 3 && l >= 4) *gw = ip_from_bytes(b + o);
        else if (opt == 6 && l >= 4) *dns = ip_from_bytes(b + o);
        o += l;
    }
    return 1;
}

static int dhcp_configure(void)
{
    static uint8_t pkt[512];
    g_udp_filter = 68;

    for (int attempt = 0; attempt < 3; attempt++) {
        /* DISCOVER */
        int n = dhcp_build(pkt, 1, 0, 0);
        g_udp_ready = 0;
        g_ip = 0;
        udp_send(0xFFFFFFFFu, 68, 67, pkt, n);
        if (!udp_wait(1500)) continue;

        int mt; uint32_t yip, sid, mask, gw, dns;
        if (!dhcp_parse(g_udp_buf, g_udp_len, &mt, &yip, &sid, &mask, &gw, &dns) || mt != 2)
            continue;

        /* REQUEST */
        n = dhcp_build(pkt, 3, yip, sid);
        g_udp_ready = 0;
        udp_send(0xFFFFFFFFu, 68, 67, pkt, n);
        if (!udp_wait(1500)) continue;

        int mt2; uint32_t yip2, sid2, mask2, gw2, dns2;
        if (!dhcp_parse(g_udp_buf, g_udp_len, &mt2, &yip2, &sid2, &mask2, &gw2, &dns2))
            continue;
        if (mt2 != 5) continue;                    /* ACK */

        g_ip   = yip2 ? yip2 : yip;
        g_mask = mask2 ? mask2 : 0xFFFFFF00u;
        g_gw   = gw2 ? gw2 : (g_ip & g_mask) | 2;
        g_dns  = dns2 ? dns2 : g_gw;
        g_udp_filter = 0;
        return 1;
    }
    g_udp_filter = 0;
    return 0;
}

/* ========================================================================== */
/* DNS                                                                        */
/* ========================================================================== */

static int dns_encode_name(uint8_t *out, const char *host)
{
    int o = 0, label = 0, labelpos = 0;
    out[o++] = 0;                                  /* length placeholder */
    labelpos = 0;
    for (int i = 0; ; i++) {
        char c = host[i];
        if (c == '.' || c == 0) {
            out[labelpos] = (uint8_t)label;
            labelpos = o++;
            out[labelpos] = 0;
            label = 0;
            if (c == 0) break;
        } else {
            out[o++] = (uint8_t)c;
            label++;
        }
    }
    out[labelpos] = 0;                             /* root terminator */
    return o;
}

int dns_resolve(const char *host, uint32_t *ip_out)
{
    /* Accept a dotted-quad directly. */
    {
        int dots = 0, ok = 1, val = 0, parts[4], pi = 0, any = 0;
        for (int i = 0; ; i++) {
            char c = host[i];
            if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); any = 1; }
            else if (c == '.') { if (pi < 4) parts[pi++] = val; val = 0; dots++; }
            else if (c == 0) { if (pi < 4) parts[pi++] = val; break; }
            else { ok = 0; break; }
        }
        if (ok && any && dots == 3 && pi == 4 &&
            parts[0] < 256 && parts[1] < 256 && parts[2] < 256 && parts[3] < 256) {
            *ip_out = ((uint32_t)parts[0] << 24) | ((uint32_t)parts[1] << 16) |
                      ((uint32_t)parts[2] << 8) | parts[3];
            return 0;
        }
    }

    static uint8_t q[512];
    uint16_t id = (uint16_t)(rdtsc() & 0xFFFF);
    uint16_t sport = (uint16_t)(50000 + (rdtsc() & 0x0FFF));

    wr16(q + 0, id);
    wr16(q + 2, 0x0100);     /* recursion desired */
    wr16(q + 4, 1);          /* qdcount */
    wr16(q + 6, 0);
    wr16(q + 8, 0);
    wr16(q + 10, 0);
    int o = 12;
    o += dns_encode_name(q + o, host);
    wr16(q + o, 1); o += 2;  /* QTYPE A */
    wr16(q + o, 1); o += 2;  /* QCLASS IN */

    g_udp_filter = sport;
    for (int attempt = 0; attempt < 3; attempt++) {
        g_udp_ready = 0;
        udp_send(g_dns, sport, 53, q, o);
        if (!udp_wait(1500)) continue;

        const uint8_t *r = g_udp_buf;
        int rl = g_udp_len;
        if (rl < 12 || rd16(r) != id) continue;
        int qd = rd16(r + 4), an = rd16(r + 6);
        int p = 12;
        for (int i = 0; i < qd && p < rl; i++) {       /* skip questions */
            while (p < rl && r[p] != 0) {
                if ((r[p] & 0xC0) == 0xC0) { p += 2; goto qdone; }
                p += r[p] + 1;
            }
            p += 1;
        qdone: p += 4;                                  /* qtype+qclass */
        }
        for (int i = 0; i < an && p < rl; i++) {        /* answers */
            if ((r[p] & 0xC0) == 0xC0) p += 2;
            else { while (p < rl && r[p] != 0) p += r[p] + 1; p += 1; }
            if (p + 10 > rl) break;
            int type = rd16(r + p);
            int rdlen = rd16(r + p + 8);
            p += 10;
            if (type == 1 && rdlen == 4 && p + 4 <= rl) {
                *ip_out = ip_from_bytes(r + p);
                g_udp_filter = 0;
                return 0;
            }
            p += rdlen;
        }
    }
    g_udp_filter = 0;
    return -1;
}

/* ========================================================================== */
/* Init / accessors                                                           */
/* ========================================================================== */

int netstack_init(void)
{
    nmemset(g_arp, 0, sizeof(g_arp));
    g_tcp.state = TCP_CLOSED;

    const uint8_t *m = nic_mac();
    if (!m) return 0;
    nmemcpy(g_mac, m, 6);

    nic_set_rx_callback(net_rx);

    if (!dhcp_configure()) {
        /* QEMU SLIRP / VirtualBox NAT defaults. */
        g_ip   = 0x0A00020F;   /* 10.0.2.15 */
        g_mask = 0xFFFFFF00;   /* 255.255.255.0 */
        g_gw   = 0x0A000202;   /* 10.0.2.2 */
        g_dns  = 0x0A000203;   /* 10.0.2.3 */
    }
    g_up = 1;
    return 1;
}

int netstack_up(void) { return g_up; }
uint32_t netstack_local_ip(void)   { return g_ip; }
uint32_t netstack_gateway_ip(void) { return g_gw; }
uint32_t netstack_dns_ip(void)     { return g_dns; }
const uint8_t *netstack_mac(void)  { return g_mac; }

void netstack_ip_str(uint32_t ip, char *out)
{
    uint8_t b[4]; ip_to_bytes(ip, b);
    int o = 0;
    for (int i = 0; i < 4; i++) {
        int v = b[i], started = 0;
        if (v >= 100) { out[o++] = (char)('0' + v / 100); v %= 100; started = 1; }
        if (v >= 10 || started) { out[o++] = (char)('0' + v / 10); v %= 10; }
        out[o++] = (char)('0' + v);
        if (i < 3) out[o++] = '.';
    }
    out[o] = 0;
}
