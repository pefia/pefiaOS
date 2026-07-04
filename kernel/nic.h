/* Generic NIC front end. Tries each network card driver pefiaOS knows
 * about (Intel e1000, since that's what VirtualBox emulates, then RTL8139
 * for QEMU) and, whichever wins, exposes one uniform send/poll/MAC
 * interface so the TCP/IP stack never has to care which one it's talking
 * to. */
#ifndef PEFIA_NIC_H
#define PEFIA_NIC_H

#include <stdint.h>

typedef void (*nic_rx_cb_t)(const uint8_t *frame, uint16_t len);

int             nic_init(void);                 /* 1 if a NIC came up */
int             nic_present(void);
const char     *nic_name(void);
const uint8_t  *nic_mac(void);
void            nic_set_rx_callback(nic_rx_cb_t cb);
int             nic_send(const void *frame, uint16_t len);
int             nic_poll(void);

#endif /* PEFIA_NIC_H */
