/* kernel/netstack.h
 * -----------------------------------------------------------------------------
 * A small, blocking, poll-driven TCP/IP stack for pefiaOS:
 *   Ethernet + ARP + IPv4 + ICMP echo + UDP (DHCP, DNS) + a single-connection
 *   TCP client. There are no interrupts wired up, so all waits pump the NIC via
 *   netstack_poll() and time out using the rdtsc-based clock.
 *
 * All IPv4 addresses in this API are HOST byte order (0x0A000202 == 10.0.2.2).
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_NETSTACK_H
#define PEFIA_NETSTACK_H

#include <stdint.h>

/* Bring up the NIC, learn the MAC, and configure IP via DHCP (falling back to
 * the well-known QEMU/VirtualBox NAT defaults). Returns 1 if the link is up. */
int  netstack_init(void);
int  netstack_up(void);
void netstack_poll(void);

uint32_t       netstack_local_ip(void);
uint32_t       netstack_gateway_ip(void);
uint32_t       netstack_dns_ip(void);
const uint8_t *netstack_mac(void);
void           netstack_ip_str(uint32_t ip, char *out16);

/* Resolve a hostname's first A record. Returns 0 and fills *ip on success. */
int  dns_resolve(const char *host, uint32_t *ip_out);

/* TCP client (one global connection at a time). */
int  tcp_connect(uint32_t ip, uint16_t port);            /* 0 on success */
int  tcp_send(const uint8_t *data, int len);             /* bytes sent or <0 */
int  tcp_recv(uint8_t *buf, int cap, int timeout_ms);    /* >0 data, 0 idle/eof */
int  tcp_closed(void);                                   /* peer FIN/RST seen */
void tcp_close(void);

#endif /* PEFIA_NETSTACK_H */
