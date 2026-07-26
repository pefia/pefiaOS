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
int  tcp_connect(uint32_t ip, uint16_t port);
int  tcp_send(const uint8_t *data, int len);
int  tcp_recv(uint8_t *buf, int cap, int timeout_ms);
int  tcp_closed(void);
void tcp_close(void);

#endif
