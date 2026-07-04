/* kernel/netstack.h
 *
 * A minimal TCP/IP stack for pefiaOS - Ethernet, ARP, IPv4, ICMP echo, UDP
 * (just enough for DHCP and DNS), and one TCP connection at a time. There's
 * no interrupt-driven RX path here: everything that "waits" is really a loop
 * calling netstack_poll() and checking the rdtsc-backed clock for a timeout.
 * Blocking-but-cooperative, basically - fine for a browser fetching one page
 * at a time, and a lot easier to reason about than real async networking.
 *
 * Every IPv4 address that crosses this API is host byte order, so
 * 10.0.2.2 is 0x0A000202, not the byte-swapped wire form.
 */
#ifndef PEFIA_NETSTACK_H
#define PEFIA_NETSTACK_H

#include <stdint.h>

/* Bring up the NIC, grab its MAC, then get an IP via DHCP - falling back to
 * the QEMU/VirtualBox NAT defaults (10.0.2.15/24, gw .2, dns .3) if nothing
 * answers. Returns 1 once we have a usable link, even in the fallback case. */
int  netstack_init(void);
int  netstack_up(void);
void netstack_poll(void);

uint32_t       netstack_local_ip(void);
uint32_t       netstack_gateway_ip(void);
uint32_t       netstack_dns_ip(void);
const uint8_t *netstack_mac(void);
void           netstack_ip_str(uint32_t ip, char *out16);

/* Resolve a hostname (or accept a dotted-quad as-is). 0 and *ip_out set
 * on success, negative if nothing answered or the name didn't resolve. */
int  dns_resolve(const char *host, uint32_t *ip_out);

/* TCP client - there's only ever one connection live at a time, which is
 * all the browser needs. */
int  tcp_connect(uint32_t ip, uint16_t port);            /* 0 on success */
int  tcp_send(const uint8_t *data, int len);             /* bytes sent, or <0 */
int  tcp_recv(uint8_t *buf, int cap, int timeout_ms);     /* >0 data, 0 idle/eof */
int  tcp_closed(void);                                    /* peer sent FIN or RST */
void tcp_close(void);

#endif /* PEFIA_NETSTACK_H */
