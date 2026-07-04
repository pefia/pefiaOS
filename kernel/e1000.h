/* Intel 8254x ("e1000") gigabit driver - polled, no interrupts wired up.
 * This is the NIC VirtualBox emulates, so it's what we get in dev VMs. */
#ifndef PEFIA_E1000_H
#define PEFIA_E1000_H

#include <stdint.h>
#include "nic.h"

int            e1000_init(void);                 /* 0 on success, negative otherwise */
int            e1000_present(void);
const uint8_t *e1000_mac(void);
void           e1000_set_rx_callback(nic_rx_cb_t cb);
int            e1000_send(const void *frame, uint16_t len);
int            e1000_poll(void);

#endif /* PEFIA_E1000_H */
