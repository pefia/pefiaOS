#include "nic.h"
#include "e1000.h"
#include "rtl8139.h"

enum { NIC_NONE = 0, NIC_E1000, NIC_RTL8139 };
static int g_kind = NIC_NONE;

int nic_init(void)
{
    /* VirtualBox's default cards are Intel e1000 / PCnet; QEMU's is RTL8139.
     * Try e1000 first, then RTL8139. */
    if (e1000_init() == 0) { g_kind = NIC_E1000; return 1; }

    Rtl8139Device dev;
    if (rtl8139_probe(&dev) && rtl8139_init(&dev) == 0) { g_kind = NIC_RTL8139; return 1; }

    g_kind = NIC_NONE;
    return 0;
}

int nic_present(void) { return g_kind != NIC_NONE; }

const char *nic_name(void)
{
    return g_kind == NIC_E1000 ? "Intel e1000" :
           g_kind == NIC_RTL8139 ? "RTL8139" : "none";
}

const uint8_t *nic_mac(void)
{
    if (g_kind == NIC_E1000) return e1000_mac();
    if (g_kind == NIC_RTL8139) { const Rtl8139Device *d = rtl8139_device(); return d ? d->mac : 0; }
    return 0;
}

void nic_set_rx_callback(nic_rx_cb_t cb)
{
    if (g_kind == NIC_E1000) e1000_set_rx_callback(cb);
    else if (g_kind == NIC_RTL8139) rtl8139_set_rx_callback(cb);
}

int nic_send(const void *frame, uint16_t len)
{
    if (g_kind == NIC_E1000) return e1000_send(frame, len);
    if (g_kind == NIC_RTL8139) return rtl8139_send(frame, len);
    return -1;
}

int nic_poll(void)
{
    if (g_kind == NIC_E1000) return e1000_poll();
    if (g_kind == NIC_RTL8139) return rtl8139_poll();
    return 0;
}
