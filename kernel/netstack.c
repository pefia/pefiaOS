#include "netstack.h"
#include "nic.h"
#include "clock.h"
#include "util.h"

/* ---- byte order helpers ----
 * Everything on the wire is big-endian; everything we keep in g_tcp/g_ip/etc
 * is host order. rd/wr do the swap at the boundary so the rest of the file
 * doesn't have to think about it. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void ip_to_bytes(uint32_t ip, uint8_t out[4])
{ out[0] = (uint8_t)(ip >> 24); out[1] = (uint8_t)(ip >> 16); out[2] = (uint8_t)(ip >> 8); out[3] = (uint8_t)ip; }
static uint32_t ip_from_bytes(const uint8_t b[4])
{ return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }

/* RFC1071 one's-complement checksum. csum_add folds a byte range into a
 * running 32-bit accumulator (so IP/UDP/TCP can add a pseudo-header and the
 * payload in two calls); csum_fold does the final carry-fold + complement. */
static uint32_t csum_add(uint32_t sum, const uint8_t *data, int len)
{
    int i = 0;
    for (; i + 1 < len; i += 2) sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if (i < len) sum += (uint32_t)(data[i] << 8);   /* odd trailing byte, padded with 0 */
    return sum;
}
static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

/* UDP and TCP both checksum a 12-byte IPv4 pseudo-header followed by the
 * segment itself; this used to be copy-pasted in both places. */
static uint16_t pseudo_csum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const uint8_t *seg, int total)
{
    uint8_t ph[12];
    ip_to_bytes(src_ip, ph + 0);
    ip_to_bytes(dst_ip, ph + 4);
    ph[8] = 0;
    ph[9] = proto;
    wr16(ph + 10, (uint16_t)total);
    uint32_t sum = csum_add(0, ph, 12);
    sum = csum_add(sum, seg, total);
    return csum_fold(sum);
}

/* ---- link state ---- */

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

static uint8_t  g_tx[2048];   /* scratch frame buffer for everything we transmit */

/* ---- ARP cache, dumb linear table, first-slot eviction ---- */
typedef struct { uint32_t ip; uint8_t mac[6]; int valid; } ArpEntry;
#define ARP_CACHE 8
static ArpEntry g_arp[ARP_CACHE];

/* ---- one captured UDP datagram at a time (DHCP + DNS both use this) ---- */
static uint8_t  g_udp_buf[1500];
static int      g_udp_len = 0;
static int      g_udp_ready = 0;
static uint16_t g_udp_filter = 0;    /* host-order dst port we're currently waiting on */

/* ---- the one TCP connection we ever have open ---- */
enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTAB, TCP_CLOSE_WAIT };
#define RCV_CAP 49152
static struct {
    int      state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    uint8_t  remote_mac[6];
    uint32_t snd_una, snd_nxt;       /* send sequence space */
    uint32_t rcv_nxt;                /* next sequence number we expect from the peer */
    int      syn_acked;
    int      fin_recv;
    int      reset;
    uint8_t  rbuf[RCV_CAP];
    int      rhead, rtail;           /* ring buffer: unread bytes live in [rtail, rhead) */
} g_tcp;

static int rcv_used(void) { int d = g_tcp.rhead - g_tcp.rtail; if (d < 0) d += RCV_CAP; return d; }
static int rcv_free(void) { return RCV_CAP - 1 - rcv_used(); }

/* ---- ethernet / arp ---- */

static void eth_send(const uint8_t dst[6], uint16_t ethertype, const uint8_t *payload, int len)
{
    kmemmove(g_tx, dst, 6);
    kmemmove(g_tx + 6, g_mac, 6);
    wr16(g_tx + 12, ethertype);
    if (payload && len > 0) kmemmove(g_tx + ETH_HDR, payload, len);
    nic_send(g_tx, (uint16_t)(ETH_HDR + len));
}

static void arp_learn(uint32_t ip, const uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { kmemmove(g_arp[i].mac, mac, 6); return; }
    for (int i = 0; i < ARP_CACHE; i++)
        if (!g_arp[i].valid) { g_arp[i].ip = ip; kmemmove(g_arp[i].mac, mac, 6); g_arp[i].valid = 1; return; }
    /* cache is full - just clobber slot 0 rather than doing anything smarter */
    g_arp[0].ip = ip; kmemmove(g_arp[0].mac, mac, 6); g_arp[0].valid = 1;
}
static int arp_lookup(uint32_t ip, uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { kmemmove(mac, g_arp[i].mac, 6); return 1; }
    return 0;
}

static void arp_send(int reply, uint32_t target_ip, const uint8_t target_mac[6])
{
    uint8_t p[28];
    wr16(p + 0, 1);           /* HTYPE: ethernet */
    wr16(p + 2, ET_IPV4);     /* PTYPE */
    p[4] = 6; p[5] = 4;       /* HLEN / PLEN */
    wr16(p + 6, reply ? 2 : 1);
    kmemmove(p + 8, g_mac, 6);
    ip_to_bytes(g_ip, p + 14);
    if (reply) kmemmove(p + 18, target_mac, 6); else kmemset(p + 18, 0, 6);
    ip_to_bytes(target_ip, p + 24);
    eth_send(reply ? target_mac : g_bcast, ET_ARP, p, 28);
}

static void handle_arp(const uint8_t *frame, int len)
{
    if (len < ETH_HDR + 28) return;
    const uint8_t *a = frame + ETH_HDR;
    uint16_t op = rd16(a + 6);
    uint32_t sender_ip = ip_from_bytes(a + 14);
    uint32_t target_ip = ip_from_bytes(a + 24);
    arp_learn(sender_ip, a + 8);
    if (op == 1 && target_ip == g_ip)         /* someone's asking who has us */
        arp_send(1, sender_ip, a + 8);
}

/* Figure out the MAC for whoever's next on the wire toward `ip` (the host
 * itself if it's on our subnet, otherwise the gateway), retrying ARP a few
 * times before giving up. Blocks by polling, since we have no interrupts. */
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

/* ---- ipv4 ---- */

static uint16_t g_ip_id = 0x1000;

static int ip_send(uint8_t proto, uint32_t dst, const uint8_t *payload, int plen)
{
    uint8_t mac[6];
    if (dst == 0xFFFFFFFFu) {
        for (int i = 0; i < 6; i++) mac[i] = 0xFF;     /* broadcast never needs ARP */
    } else if (!resolve_mac(dst, mac)) {
        return -1;
    }

    uint8_t hdr[IP_HDR];
    hdr[0] = 0x45;                 /* version 4, IHL 5 (no options) */
    hdr[1] = 0x00;                 /* DSCP/ECN, we don't care */
    wr16(hdr + 2, (uint16_t)(IP_HDR + plen));
    wr16(hdr + 4, g_ip_id++);
    wr16(hdr + 6, 0x4000);         /* don't-fragment; we never fragment anyway */
    hdr[8] = 64;                   /* TTL */
    hdr[9] = proto;
    wr16(hdr + 10, 0);             /* checksum, filled in below */
    ip_to_bytes(g_ip, hdr + 12);
    ip_to_bytes(dst, hdr + 16);
    wr16(hdr + 10, csum_fold(csum_add(0, hdr, IP_HDR)));

    /* Stitch header + payload together in one buffer before handing it to
     * ethernet - keeps eth_send() simple (one contiguous payload). */
    static uint8_t datagram[1600];
    kmemmove(datagram, hdr, IP_HDR);
    if (payload && plen > 0) kmemmove(datagram + IP_HDR, payload, plen);
    eth_send(mac, ET_IPV4, datagram, IP_HDR + plen);
    return 0;
}

/* ---- icmp: just enough to answer a ping ---- */

static void handle_icmp(uint32_t src, const uint8_t *p, int len)
{
    if (len < 8) return;
    if (p[0] != 8) return;                       /* only echo requests, ignore the rest */
    static uint8_t reply[1500];
    if (len > (int)sizeof(reply)) return;
    kmemmove(reply, p, len);
    reply[0] = 0;                                /* type 0 = echo reply */
    wr16(reply + 2, 0);
    wr16(reply + 2, csum_fold(csum_add(0, reply, len)));
    ip_send(IPPROTO_ICMP, src, reply, len);
}

/* ---- udp ---- */

static void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                     const uint8_t *data, int len)
{
    static uint8_t seg[1500];
    int total = 8 + len;
    wr16(seg + 0, src_port);
    wr16(seg + 2, dst_port);
    wr16(seg + 4, (uint16_t)total);
    wr16(seg + 6, 0);
    if (data && len > 0) kmemmove(seg + 8, data, len);

    uint16_t ck = pseudo_csum(g_ip, dst_ip, IPPROTO_UDP, seg, total);
    if (ck == 0) ck = 0xFFFF;    /* 0 means "no checksum" on the wire, so dodge it */
    wr16(seg + 6, ck);

    ip_send(IPPROTO_UDP, dst_ip, seg, total);
}

static void handle_udp(uint32_t src, const uint8_t *p, int len)
{
    (void)src;
    if (len < 8) return;
    uint16_t dport = rd16(p + 2);
    int dlen = (int)rd16(p + 4) - 8;
    if (dlen < 0 || dlen > len - 8) dlen = len - 8;   /* don't trust the length field blindly */
    if (dport == g_udp_filter && dlen <= (int)sizeof(g_udp_buf)) {
        kmemmove(g_udp_buf, p + 8, dlen);
        g_udp_len = dlen;
        g_udp_ready = 1;
    }
}

/* Spin (polling the NIC) until a datagram lands on the port we're filtering
 * for, or we run out of patience. */
static int udp_wait(int timeout_ms)
{
    uint32_t t0 = clock_ms();
    while ((uint32_t)(clock_ms() - t0) < (uint32_t)timeout_ms) {
        netstack_poll();
        if (g_udp_ready) return 1;
    }
    return 0;
}

/* ---- tcp ----
 * Deliberately simple: no retransmit queue beyond "resend the whole chunk
 * and wait again," no SACK, no out-of-order reassembly. Good enough for a
 * single HTTP request/response over a NAT'd link, which is the only thing
 * that talks TCP on this OS right now. */

static void tcp_xmit(uint8_t flags, const uint8_t *data, int dlen)
{
    static uint8_t seg[1600];
    int hlen = 20;
    if (flags & 0x02) {                  /* SYN: advertise an MSS of 1460 */
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
    wr16(seg + 16, 0);                   /* checksum, filled below */
    wr16(seg + 18, 0);                   /* urgent pointer, unused */
    if (data && dlen > 0) kmemmove(seg + hlen, data, dlen);

    int total = hlen + dlen;
    wr16(seg + 16, pseudo_csum(g_tcp.local_ip, g_tcp.remote_ip, IPPROTO_TCP, seg, total));

    /* remote MAC was already resolved back in tcp_connect() */
    ip_send(IPPROTO_TCP, g_tcp.remote_ip, seg, total);
}

static void tcp_ack(void) { tcp_xmit(0x10, 0, 0); }

static void handle_tcp(uint32_t src, const uint8_t *p, int len)
{
    if (len < 20) return;
    uint16_t sport = rd16(p + 0), dport = rd16(p + 2);
    if (g_tcp.state == TCP_CLOSED) return;
    if (src != g_tcp.remote_ip || sport != g_tcp.remote_port || dport != g_tcp.local_port) return;

    uint32_t rseq = rd32(p + 4);
    uint32_t rack = rd32(p + 8);
    uint8_t  off = (uint8_t)((p[12] >> 4) * 4);
    uint8_t  flags = p[13];
    const uint8_t *data = p + off;
    int dlen = len - off;
    if (dlen < 0) dlen = 0;

    if (flags & 0x04) { g_tcp.reset = 1; g_tcp.state = TCP_CLOSED; return; }   /* RST: we're done */

    if (g_tcp.state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12) {           /* SYN+ACK */
            g_tcp.rcv_nxt = rseq + 1;
            g_tcp.snd_una = rack;
            g_tcp.state = TCP_ESTAB;
            g_tcp.syn_acked = 1;
            tcp_ack();
        }
        return;
    }

    if (flags & 0x10) {                          /* ACK: pull snd_una forward if this is newer */
        if (rack - g_tcp.snd_una <= (g_tcp.snd_nxt - g_tcp.snd_una))
            g_tcp.snd_una = rack;
    }

    /* We only accept data that arrives exactly in order - anything else
     * gets a duplicate ACK and is otherwise dropped on the floor. */
    if (dlen > 0 && rseq == g_tcp.rcv_nxt) {
        int n = dlen; if (n > rcv_free()) n = rcv_free();
        for (int i = 0; i < n; i++) {
            g_tcp.rbuf[g_tcp.rhead] = data[i];
            g_tcp.rhead = (g_tcp.rhead + 1) % RCV_CAP;
        }
        g_tcp.rcv_nxt += n;
        tcp_ack();
    } else if (dlen > 0 && rseq != g_tcp.rcv_nxt) {
        tcp_ack();                               /* stale retransmit or reordered segment */
    }

    if (flags & 0x01) {                          /* FIN, but only once it's actually next in line */
        if (rseq + (uint32_t)dlen == g_tcp.rcv_nxt) {
            g_tcp.rcv_nxt += 1;
            g_tcp.fin_recv = 1;
            g_tcp.state = TCP_CLOSE_WAIT;
            tcp_ack();
        }
    }
}

int tcp_connect(uint32_t ip, uint16_t port)
{
    kmemset(&g_tcp, 0, sizeof(g_tcp));
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
        g_tcp.snd_nxt = isn;                     /* the SYN itself consumes seq isn */
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
        while ((uint32_t)(clock_ms() - t0) < 300) netstack_poll();   /* give the FIN a chance to land */
    }
    g_tcp.state = TCP_CLOSED;
}

/* ---- rx dispatch: this is the NIC's callback, called from netstack_poll() ---- */

static void net_rx(const uint8_t *frame, uint16_t len)
{
    if (len < ETH_HDR) return;
    uint16_t ethertype = rd16(frame + 12);

    if (ethertype == ET_ARP) { handle_arp(frame, len); return; }
    if (ethertype != ET_IPV4) return;
    if (len < ETH_HDR + IP_HDR) return;

    const uint8_t *ip = frame + ETH_HDR;
    int ihl = (ip[0] & 0x0F) * 4;
    /* IHL is attacker-controlled (it's just 4 bits off the wire) - make sure
     * the header it claims actually fits inside the frame we received
     * before we start indexing off the end of it. */
    if (ihl < 20 || ihl > len - ETH_HDR) return;

    uint8_t proto = ip[9];
    uint32_t src = ip_from_bytes(ip + 12);
    uint32_t dst = ip_from_bytes(ip + 16);
    if (dst != g_ip && dst != 0xFFFFFFFFu) {
        /* still take broadcast traffic while we're waiting on DHCP */
        if (g_ip != 0) return;
    }
    int total = rd16(ip + 2);
    int plen = total - ihl;
    int available = len - ETH_HDR - ihl;
    if (plen < 0 || plen > available) plen = available;
    const uint8_t *payload = ip + ihl;

    if (proto == IPPROTO_ICMP) handle_icmp(src, payload, plen);
    else if (proto == IPPROTO_UDP) handle_udp(src, payload, plen);
    else if (proto == IPPROTO_TCP) handle_tcp(src, payload, plen);
}

void netstack_poll(void) { nic_poll(); }

/* ---- dhcp ----
 * Discover/Request/Ack, no rebinding or renewal - we grab a lease once at
 * boot and just keep using it. Good enough for a machine that's up for a
 * browsing session, not a long-running server. */

static uint32_t g_xid = 0x50454649;   /* "PEFI" - just needs to be recognizable in a packet capture */

static int dhcp_build(uint8_t *b, int msgtype, uint32_t req_ip, uint32_t server_id)
{
    kmemset(b, 0, 300);
    b[0] = 1; b[1] = 1; b[2] = 6; b[3] = 0;       /* op=request, htype=eth, hlen=6, hops=0 */
    wr32(b + 4, g_xid);
    wr16(b + 10, 0x8000);                          /* ask for a broadcast reply, we have no IP yet */
    kmemmove(b + 28, g_mac, 6);                    /* chaddr */
    wr32(b + 236, 0x63825363);                     /* DHCP magic cookie */

    int o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = (uint8_t)msgtype;                          /* message type */
    b[o++] = 55; b[o++] = 4; b[o++] = 1; b[o++] = 3; b[o++] = 6; b[o++] = 15;    /* param req list */
    if (req_ip)    { b[o++] = 50; b[o++] = 4; ip_to_bytes(req_ip, b + o); o += 4; }     /* requested IP */
    if (server_id) { b[o++] = 54; b[o++] = 4; ip_to_bytes(server_id, b + o); o += 4; }  /* server id */
    b[o++] = 61; b[o++] = 7; b[o++] = 1; kmemmove(b + o, g_mac, 6); o += 6;             /* client id */
    b[o++] = 255;

    return o < 300 ? 300 : o;    /* BOOTP wants at least a 300-byte body, padding is free */
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
        if (opt == 0) continue;         /* pad */
        if (o >= len) break;
        uint8_t optlen = b[o++];
        if (o + optlen > len) break;    /* truncated option, bail rather than read past the buffer */
        if (opt == 53 && optlen == 1) *msgtype = b[o];
        else if (opt == 54 && optlen == 4) *sid = ip_from_bytes(b + o);
        else if (opt == 1 && optlen == 4) *mask = ip_from_bytes(b + o);
        else if (opt == 3 && optlen >= 4) *gw = ip_from_bytes(b + o);
        else if (opt == 6 && optlen >= 4) *dns = ip_from_bytes(b + o);
        o += optlen;
    }
    return 1;
}

static int dhcp_configure(void)
{
    static uint8_t pkt[512];
    g_udp_filter = 68;

    for (int attempt = 0; attempt < 3; attempt++) {
        int n = dhcp_build(pkt, 1 /* DISCOVER */, 0, 0);
        g_udp_ready = 0;
        g_ip = 0;
        udp_send(0xFFFFFFFFu, 68, 67, pkt, n);
        if (!udp_wait(1500)) continue;

        int mt; uint32_t yip, sid, mask, gw, dns;
        if (!dhcp_parse(g_udp_buf, g_udp_len, &mt, &yip, &sid, &mask, &gw, &dns) || mt != 2 /* OFFER */)
            continue;

        n = dhcp_build(pkt, 3 /* REQUEST */, yip, sid);
        g_udp_ready = 0;
        udp_send(0xFFFFFFFFu, 68, 67, pkt, n);
        if (!udp_wait(1500)) continue;

        int mt2; uint32_t yip2, sid2, mask2, gw2, dns2;
        if (!dhcp_parse(g_udp_buf, g_udp_len, &mt2, &yip2, &sid2, &mask2, &gw2, &dns2))
            continue;
        if (mt2 != 5 /* ACK */) continue;

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

/* ---- dns: one A-record query, no caching, no recursion of our own ---- */

/* Encode `host` as DNS labels into out[0..cap). Returns the encoded length,
 * or -1 if it wouldn't fit - a hostname would have to be enormous to hit
 * that, but there's no reason to trust it blindly either. */
static int dns_encode_name(uint8_t *out, int cap, const char *host)
{
    if (cap < 1) return -1;
    int o = 0, label = 0, labelpos = 0;
    out[o++] = 0;                                  /* length byte for the first label, patched below */
    for (int i = 0; ; i++) {
        char c = host[i];
        if (c == '.' || c == 0) {
            out[labelpos] = (uint8_t)label;
            if (o >= cap) return -1;
            labelpos = o++;
            out[labelpos] = 0;
            label = 0;
            if (c == 0) break;
        } else {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)c;
            label++;
        }
    }
    out[labelpos] = 0;                             /* root terminator */
    return o;
}

int dns_resolve(const char *host, uint32_t *ip_out)
{
    /* A dotted-quad doesn't need a round trip - just parse it. */
    {
        int dots = 0, ok = 1, val = 0, parts[4], pi = 0, saw_digit = 0;
        for (int i = 0; ; i++) {
            char c = host[i];
            if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); saw_digit = 1; }
            else if (c == '.') { if (pi < 4) parts[pi++] = val; val = 0; dots++; }
            else if (c == 0) { if (pi < 4) parts[pi++] = val; break; }
            else { ok = 0; break; }
        }
        if (ok && saw_digit && dots == 3 && pi == 4 &&
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
    wr16(q + 2, 0x0100);     /* recursion desired, standard query */
    wr16(q + 4, 1);          /* qdcount */
    wr16(q + 6, 0);
    wr16(q + 8, 0);
    wr16(q + 10, 0);

    int o = 12;
    int namelen = dns_encode_name(q + o, (int)sizeof(q) - o, host);
    if (namelen < 0) return -1;
    o += namelen;
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
        for (int i = 0; i < qd && p < rl; i++) {       /* skip over the question section */
            while (p < rl && r[p] != 0) {
                if ((r[p] & 0xC0) == 0xC0) { p += 2; goto question_done; }
                p += r[p] + 1;
            }
            p += 1;
        question_done: p += 4;                          /* qtype + qclass */
        }
        for (int i = 0; i < an && p < rl; i++) {        /* walk the answer records */
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

/* ---- init / accessors ---- */

int netstack_init(void)
{
    kmemset(g_arp, 0, sizeof(g_arp));
    g_tcp.state = TCP_CLOSED;

    const uint8_t *mac = nic_mac();
    if (!mac) return 0;
    kmemmove(g_mac, mac, 6);

    nic_set_rx_callback(net_rx);

    if (!dhcp_configure()) {
        /* Nobody answered - assume we're under QEMU SLIRP or VirtualBox NAT
         * and use their well-known defaults instead of giving up entirely. */
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
