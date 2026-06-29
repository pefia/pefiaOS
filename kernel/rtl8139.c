#include "rtl8139.h"
#include "io.h"
#include "heap.h"

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

/* Register map (offset from I/O base) */
#define REG_IDR0      0x00
#define REG_MAR0      0x08
#define REG_TXSTATUS0 0x10
#define REG_TXADDR0   0x20
#define REG_RXBUF     0x30
#define REG_CHIPCMD   0x37
#define REG_RXBUFTAIL 0x38
#define REG_RXBUFHEAD 0x3A
#define REG_INTRMASK  0x3C
#define REG_INTRSTAT  0x3E
#define REG_TXCONFIG  0x40
#define REG_RXCONFIG  0x44
#define REG_CFG9346   0x50
#define REG_CONFIG1   0x52
#define REG_MPC       0x4C
#define REG_BMCR      0x62

/* Chip command bits */
#define CMD_RESET 0x10
#define CMD_RX_EN 0x08
#define CMD_TX_EN 0x04

/* Rx status bits */
#define RX_OK          0x0001
#define RX_BAD_ALIGN   0x0002
#define RX_CRC_ERR     0x0004
#define RX_LONG_ERR    0x0008
#define RX_RUNT_ERR    0x0010
#define RX_ISE_ERR     0x0020
#define RX_BAR_ERR     0x2000

/* RxConfig: accept broadcast + physical match + wrap + 32K ring */
#define RXCFG_AAP      (1u << 0)
#define RXCFG_APM      (1u << 1)
#define RXCFG_AM       (1u << 2)
#define RXCFG_AB       (1u << 3)
#define RXCFG_WRAP     (1u << 7)
/* RBLEN field (bits 12:11): 00=8K 01=16K 10=32K 11=64K (+16 byte header). */
#define RXCFG_RBLEN_32K (2u << 11)
#define RXCFG_FTH_NONE (7u << 13)

#define RX_BUF_BYTES   (32 * 1024)
#define RX_BUF_PAD     2048
#define RX_BUF_SIZE    (RX_BUF_BYTES + RX_BUF_PAD)

#define TX_BUF_SIZE    2048
#define TX_DESC_COUNT  4

static Rtl8139Device g_dev;
static uint8_t *g_txbuf[TX_DESC_COUNT];
static rtl8139_rx_cb_t g_rx_cb = 0;
static uint32_t g_rx_off = 0;

static inline uint8_t r8(uint16_t io, uint16_t reg)  { return inb((uint16_t)(io + reg)); }
static inline uint16_t r16(uint16_t io, uint16_t reg){ return inw((uint16_t)(io + reg)); }
static inline uint32_t r32(uint16_t io, uint16_t reg){ return inl((uint16_t)(io + reg)); }
static inline void w8(uint16_t io, uint16_t reg, uint8_t v)   { outb((uint16_t)(io + reg), v); }
static inline void w16(uint16_t io, uint16_t reg, uint16_t v) { outw((uint16_t)(io + reg), v); }
static inline void w32(uint16_t io, uint16_t reg, uint32_t v) { outl((uint16_t)(io + reg), v); }

static uint32_t virt_to_phys(void *p)
{
    return (uint32_t)(uintptr_t)p;
}

void rtl8139_set_rx_callback(rtl8139_rx_cb_t cb)
{
    g_rx_cb = cb;
}

const Rtl8139Device *rtl8139_device(void)
{
    return g_dev.present ? &g_dev : 0;
}

int rtl8139_probe(Rtl8139Device *out)
{
    PciDeviceInfo dev;
    if (!pci_find_device_by_id(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &dev)) return 0;

    if (out) {
        out->present = 1;
        out->bus = dev.bus;
        out->slot = dev.slot;
        out->func = dev.func;
        out->io_base = 0;
        out->irq_line = 0;
        out->mac[0] = out->mac[1] = out->mac[2] = 0;
        out->mac[3] = out->mac[4] = out->mac[5] = 0;
        out->rx_buf = 0;
        out->rx_buf_phys = 0;
        out->tx_cur = 0;
    }
    return 1;
}

int rtl8139_init(Rtl8139Device *dev)
{
    if (!dev) return -1;

    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x0005; /* IO space + bus master */
    /* config write helper absent in pci.c right now; write through CF8/CFC directly */
    outl(0xCF8, 0x80000000u | ((uint32_t)dev->bus << 16) | ((uint32_t)dev->slot << 11) | ((uint32_t)dev->func << 8) | 0x04);
    outl(0xCFC, (uint32_t)cmd);

    uint32_t bar0 = pci_config_read32(dev->bus, dev->slot, dev->func, 0x10);
    if ((bar0 & 0x1) == 0) return -2;
    dev->io_base = (uint16_t)(bar0 & 0xFFFC);

    dev->irq_line = pci_config_read8(dev->bus, dev->slot, dev->func, 0x3C);

    w8(dev->io_base, REG_CONFIG1, 0x00);
    w8(dev->io_base, REG_CHIPCMD, CMD_RESET);
    for (int i = 0; i < 100000; i++) {
        if ((r8(dev->io_base, REG_CHIPCMD) & CMD_RESET) == 0) break;
        io_wait();
    }

    dev->rx_buf = (uint8_t *)kmalloc(RX_BUF_SIZE + 16);
    if (!dev->rx_buf) return -3;
    uintptr_t rxp = (uintptr_t)dev->rx_buf;
    rxp = (rxp + 15u) & ~((uintptr_t)15u);
    dev->rx_buf = (uint8_t *)rxp;
    dev->rx_buf_phys = virt_to_phys(dev->rx_buf);

    for (int i = 0; i < TX_DESC_COUNT; i++) {
        g_txbuf[i] = (uint8_t *)kmalloc(TX_BUF_SIZE + 16);
        if (!g_txbuf[i]) return -4;
        uintptr_t txp = (uintptr_t)g_txbuf[i];
        txp = (txp + 15u) & ~((uintptr_t)15u);
        g_txbuf[i] = (uint8_t *)txp;
        w32(dev->io_base, (uint16_t)(REG_TXADDR0 + i * 4), virt_to_phys(g_txbuf[i]));
    }

    w32(dev->io_base, REG_RXBUF, dev->rx_buf_phys);
    w16(dev->io_base, REG_RXBUFHEAD, 0);
    w16(dev->io_base, REG_RXBUFTAIL, 0);
    g_rx_off = 0;

    for (int i = 0; i < 8; i++) dev->mac[i % 6] = r8(dev->io_base, (uint16_t)(REG_IDR0 + i));
    for (int i = 0; i < 8; i++) w8(dev->io_base, (uint16_t)(REG_MAR0 + i), 0x00);

    w32(dev->io_base, REG_MPC, 0);

    w32(dev->io_base, REG_RXCONFIG, RXCFG_AAP | RXCFG_APM | RXCFG_AB | RXCFG_WRAP | RXCFG_RBLEN_32K | RXCFG_FTH_NONE);
    w32(dev->io_base, REG_TXCONFIG, 0x03000700u);

    w16(dev->io_base, REG_INTRSTAT, 0xFFFF);
    w16(dev->io_base, REG_INTRMASK, 0x0005);

    w8(dev->io_base, REG_CHIPCMD, CMD_RX_EN | CMD_TX_EN);

    dev->tx_cur = 0;

    g_dev = *dev;
    return 0;
}

int rtl8139_send(const void *frame, uint16_t len)
{
    if (!g_dev.present || !frame) return -1;
    if (len < 60) len = 60;
    if (len > TX_BUF_SIZE) return -2;

    uint8_t idx = g_dev.tx_cur & 0x3;
    uint8_t *tx = g_txbuf[idx];
    const uint8_t *src = (const uint8_t *)frame;
    for (uint16_t i = 0; i < len; i++) tx[i] = src[i];

    w32(g_dev.io_base, (uint16_t)(REG_TXSTATUS0 + idx * 4), (uint32_t)len);
    g_dev.tx_cur = (uint8_t)((g_dev.tx_cur + 1) & 0x3);
    return len;
}

int rtl8139_poll(void)
{
    if (!g_dev.present) return 0;

    uint16_t isr = r16(g_dev.io_base, REG_INTRSTAT);
    if (isr) w16(g_dev.io_base, REG_INTRSTAT, isr);

    int delivered = 0;
    /* CHIPCMD bit0 (BUFE) set => receive ring empty. */
    while ((r8(g_dev.io_base, REG_CHIPCMD) & 0x01) == 0) {
        uint8_t *p = g_dev.rx_buf + g_rx_off;
        uint16_t rs = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        uint16_t rl = (uint16_t)(p[2] | ((uint16_t)p[3] << 8)); /* incl 4B CRC */

        if (rl == 0xFFFF) break;               /* still being DMA'd by the NIC */

        if ((rs & RX_OK) && rl >= 4 && rl <= 1600) {
            uint16_t frame_len = (uint16_t)(rl - 4);
            if (g_rx_cb) g_rx_cb(p + 4, frame_len);
            delivered++;
        } else {
            /* Bad packet: reset the ring to recover. */
            w8(g_dev.io_base, REG_CHIPCMD, CMD_TX_EN);
            w8(g_dev.io_base, REG_CHIPCMD, CMD_RX_EN | CMD_TX_EN);
            g_rx_off = 0;
            w16(g_dev.io_base, REG_RXBUFTAIL, (uint16_t)(0 - 16));
            break;
        }

        /* Advance past header(4) + packet(rl), 4-byte aligned, wrap at ring size. */
        g_rx_off = (uint32_t)((g_rx_off + rl + 4 + 3) & ~3u);
        g_rx_off %= RX_BUF_BYTES;
        w16(g_dev.io_base, REG_RXBUFTAIL, (uint16_t)(g_rx_off - 16));
    }

    return delivered;
}
