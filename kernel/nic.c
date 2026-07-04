#include "nic.h"
#include "e1000.h"
#include "rtl8139.h"

enum { NIC_NONE = 0, NIC_E1000, NIC_RTL8139 };
static int active_nic = NIC_NONE;

int nic_init(void)
{
    /* e1000 first since that's VirtualBox's default; fall back to RTL8139
     * for QEMU. Real hardware with something else entirely is out of luck
     * until someone writes a driver for it. */
    if (e1000_init() == 0) { active_nic = NIC_E1000; return 1; }

    Rtl8139Device dev;
    if (rtl8139_probe(&dev) && rtl8139_init(&dev) == 0) { active_nic = NIC_RTL8139; return 1; }

    active_nic = NIC_NONE;
    return 0;
}

int nic_present(void) { return active_nic != NIC_NONE; }

const char *nic_name(void)
{
    return active_nic == NIC_E1000 ? "Intel e1000" :
           active_nic == NIC_RTL8139 ? "RTL8139" : "none";
}

const uint8_t *nic_mac(void)
{
    if (active_nic == NIC_E1000) return e1000_mac();
    if (active_nic == NIC_RTL8139) { const Rtl8139Device *d = rtl8139_device(); return d ? d->mac : 0; }
    return 0;
}

void nic_set_rx_callback(nic_rx_cb_t cb)
{
    if (active_nic == NIC_E1000) e1000_set_rx_callback(cb);
    else if (active_nic == NIC_RTL8139) rtl8139_set_rx_callback(cb);
}

int nic_send(const void *frame, uint16_t len)
{
    if (active_nic == NIC_E1000) return e1000_send(frame, len);
    if (active_nic == NIC_RTL8139) return rtl8139_send(frame, len);
    return -1;
}

int nic_poll(void)
{
    if (active_nic == NIC_E1000) return e1000_poll();
    if (active_nic == NIC_RTL8139) return rtl8139_poll();
    return 0;
}
