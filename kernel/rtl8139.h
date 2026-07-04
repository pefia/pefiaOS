#ifndef PEFIA_RTL8139_H
#define PEFIA_RTL8139_H

#include <stdint.h>
#include "pci.h"

/* Everything nic.c needs to know about the card we found. rx_buf/rx_buf_phys
 * are filled in once rtl8139_init() has actually allocated the DMA ring -
 * they're garbage (well, zero) before that. */
typedef struct {
    int      present;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t io_base;
    uint8_t  irq_line;
    uint8_t  mac[6];

    uint8_t *rx_buf;
    uint32_t rx_buf_phys;
    uint8_t  tx_cur;
} Rtl8139Device;

typedef void (*rtl8139_rx_cb_t)(const uint8_t *frame, uint16_t len);

int rtl8139_probe(Rtl8139Device *out);
int rtl8139_init(Rtl8139Device *dev);
int rtl8139_send(const void *frame, uint16_t len);
int rtl8139_poll(void);
void rtl8139_set_rx_callback(rtl8139_rx_cb_t cb);
const Rtl8139Device *rtl8139_device(void);

#endif
