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

#endif
