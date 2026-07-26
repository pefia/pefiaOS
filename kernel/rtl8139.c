#include "rtl8139.h"
#include "io.h"
#include "heap.h"
#include "util.h"

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

/* I/O-mapped register offsets. Nothing fancy - this chip predates MMIO BARs
 * being the norm, so everything hangs off the legacy port BAR. */
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

#define CMD_RESET 0x10
#define CMD_RX_EN 0x08
#define CMD_TX_EN 0x04
#define CMD_BUFE  0x01   /* set when the rx ring has nothing left to hand us */

/* Per-packet status word sitting in front of every received frame. */
#define RX_OK          0x0001
#define RX_BAD_ALIGN   0x0002
#define RX_CRC_ERR     0x0004
#define RX_LONG_ERR    0x0008
#define RX_RUNT_ERR    0x0010
#define RX_ISE_ERR     0x0020
#define RX_BAR_ERR     0x2000

/* RxConfig: take broadcast + our own unicast address + wrap the ring on
 * overrun, 32K ring size, no early-fetch threshold. */
#define RXCFG_AAP      (1u << 0)
#define RXCFG_APM      (1u << 1)
#define RXCFG_AM       (1u << 2)
#define RXCFG_AB       (1u << 3)
#define RXCFG_WRAP     (1u << 7)
/* RBLEN (bits 12:11): 00=8K 01=16K 10=32K 11=64K, plus a fixed 16-byte pad. */
#define RXCFG_RBLEN_32K (2u << 11)
#define RXCFG_FTH_NONE (7u << 13)

#define RX_RING_BYTES  (32 * 1024)
#define RX_RING_SLOP   2048   /* the datasheet wants room past the ring for the largest frame */
#define RX_ALLOC_SIZE  (RX_RING_BYTES + RX_RING_SLOP)

#define TX_SLOT_SIZE   2048
#define TX_SLOT_COUNT  4
#define TX_OWN         0x2000   /* TSD bit: chip sets it once it's done DMA-ing the slot */

static Rtl8139Device g_card;
static uint8_t *g_tx_slot[TX_SLOT_COUNT];
static rtl8139_rx_cb_t g_rx_cb = 0;
static uint32_t g_rx_cursor = 0;

static inline uint8_t  get8(uint16_t io, uint16_t reg)  { return inb((uint16_t)(io + reg)); }
static inline uint16_t get16(uint16_t io, uint16_t reg) { return inw((uint16_t)(io + reg)); }
static inline uint32_t get32(uint16_t io, uint16_t reg) { return inl((uint16_t)(io + reg)); }
static inline void put8(uint16_t io, uint16_t reg, uint8_t v)   { outb((uint16_t)(io + reg), v); }
static inline void put16(uint16_t io, uint16_t reg, uint16_t v) { outw((uint16_t)(io + reg), v); }
static inline void put32(uint16_t io, uint16_t reg, uint32_t v) { outl((uint16_t)(io + reg), v); }

/* We're identity-mapped, so this is really just a cast - but keeping it as
 * its own function means the one place that'd need to change if that ever
 * stops being true is obvious. */
static uint32_t phys_of(void *ptr)
{
    return (uint32_t)(uintptr_t)ptr;
}

/* kmalloc doesn't promise any particular alignment, and the NIC wants its
 * DMA buffers on a 16-byte boundary, so we always ask for a little extra
 * and slide the pointer forward. */
static uint8_t *alloc_dma_buf(uint32_t size)
{
    uint8_t *raw = (uint8_t *)kmalloc(size + 16);
    if (!raw) return 0;
    uintptr_t aligned = ((uintptr_t)raw + 15u) & ~(uintptr_t)15u;
    return (uint8_t *)aligned;
}

void rtl8139_set_rx_callback(rtl8139_rx_cb_t cb)
{
    g_rx_cb = cb;
}

const Rtl8139Device *rtl8139_device(void)
{
    return g_card.present ? &g_card : 0;
}

int rtl8139_probe(Rtl8139Device *out)
{
    PciDeviceInfo pci_dev;
    if (!pci_find_device_by_id(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &pci_dev)) return 0;

    if (out) {
        kmemset(out, 0, sizeof(*out));
        out->present = 1;
        out->bus = pci_dev.bus;
        out->slot = pci_dev.slot;
        out->func = pci_dev.func;
    }
    return 1;
}

/* Flip on I/O-space access and bus mastering in the PCI command register.
 * pci.c only exposes reads right now, so we poke CF8/CFC ourselves for the
 * one write we actually need. */
static void enable_pci_io_and_master(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t cmd = pci_config_read16(bus, slot, func, 0x04);
    cmd |= 0x0005;
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | 0x04);
    outl(0xCFC, (uint32_t)cmd);
}

static int reset_chip(uint16_t io_base)
{
    put8(io_base, REG_CONFIG1, 0x00);
    put8(io_base, REG_CHIPCMD, CMD_RESET);
    for (int spins = 0; spins < 100000; spins++) {
        if ((get8(io_base, REG_CHIPCMD) & CMD_RESET) == 0) return 0;
        io_wait();
    }
    return -1; /* chip never cleared the reset bit - caller decides how much to care */
}

int rtl8139_init(Rtl8139Device *dev)
{
    if (!dev) return -1;

    enable_pci_io_and_master(dev->bus, dev->slot, dev->func);

    uint32_t bar0 = pci_config_read32(dev->bus, dev->slot, dev->func, 0x10);
    if ((bar0 & 0x1) == 0) return -2; /* BAR0 should be I/O-space; if not, wrong chip or weird BIOS */
    dev->io_base = (uint16_t)(bar0 & 0xFFFC);
    dev->irq_line = pci_config_read8(dev->bus, dev->slot, dev->func, 0x3C);

    reset_chip(dev->io_base); /* worst case we proceed with a half-reset card; init below still reprograms everything */

    dev->rx_buf = alloc_dma_buf(RX_ALLOC_SIZE);
    if (!dev->rx_buf) return -3;
    dev->rx_buf_phys = phys_of(dev->rx_buf);

    for (int slot = 0; slot < TX_SLOT_COUNT; slot++) {
        g_tx_slot[slot] = alloc_dma_buf(TX_SLOT_SIZE);
        if (!g_tx_slot[slot]) return -4;
        put32(dev->io_base, (uint16_t)(REG_TXADDR0 + slot * 4), phys_of(g_tx_slot[slot]));
    }

    put32(dev->io_base, REG_RXBUF, dev->rx_buf_phys);
    put16(dev->io_base, REG_RXBUFHEAD, 0);
    put16(dev->io_base, REG_RXBUFTAIL, 0);
    g_rx_cursor = 0;

    /* IDR0..IDR5 hold the burned-in station address, one byte per register */
    for (int i = 0; i < 6; i++) dev->mac[i] = get8(dev->io_base, (uint16_t)(REG_IDR0 + i));
    for (int i = 0; i < 8; i++) put8(dev->io_base, (uint16_t)(REG_MAR0 + i), 0x00);

    put32(dev->io_base, REG_MPC, 0);

    put32(dev->io_base, REG_RXCONFIG,
          RXCFG_AAP | RXCFG_APM | RXCFG_AB | RXCFG_WRAP | RXCFG_RBLEN_32K | RXCFG_FTH_NONE);
    put32(dev->io_base, REG_TXCONFIG, 0x03000700u);

    put16(dev->io_base, REG_INTRSTAT, 0xFFFF);
    put16(dev->io_base, REG_INTRMASK, 0x0005);

    put8(dev->io_base, REG_CHIPCMD, CMD_RX_EN | CMD_TX_EN);

    dev->tx_cur = 0;
    g_card = *dev;
    return 0;
}

int rtl8139_send(const void *frame, uint16_t len)
{
    if (!g_card.present || !frame) return -1;
    if (len < 60) len = 60;          /* Ethernet minimum payload, chip won't pad it for us */
    if (len > TX_SLOT_SIZE) return -2;

    uint8_t slot = g_card.tx_cur & (TX_SLOT_COUNT - 1);
    uint16_t tsd = (uint16_t)(REG_TXSTATUS0 + slot * 4);

    /* Only four descriptors, so a burst of sends can lap the ring while
     * the chip is still DMA-ing this slot - and rewriting TXSTATUS mid-DMA
     * is undefined. Spin (bounded, same style as ata.c) until the chip
     * hands the slot back; on timeout drop the frame and let the caller's
     * retry path deal with it rather than corrupt what's on the wire. */
    int owned = 0;
    for (int spins = 0; spins < 100000; spins++) {
        if (get32(g_card.io_base, tsd) & TX_OWN) { owned = 1; break; }
        io_wait();
    }
    if (!owned) return -3;

    kmemmove(g_tx_slot[slot], frame, len);

    put32(g_card.io_base, tsd, (uint32_t)len);
    g_card.tx_cur = (uint8_t)((slot + 1) & (TX_SLOT_COUNT - 1));
    return len;
}

/* Ring got out of sync (bad checksum, runt, whatever) - the datasheet's
 * prescribed recovery is a TX-only cycle followed by re-enabling RX with
 * the tail nudged back by 16 (the header size), then start reading from
 * byte 0 again. */
static void recover_rx_ring(uint16_t io_base)
{
    put8(io_base, REG_CHIPCMD, CMD_TX_EN);
    put8(io_base, REG_CHIPCMD, CMD_RX_EN | CMD_TX_EN);
    g_rx_cursor = 0;
    put16(io_base, REG_RXBUFTAIL, (uint16_t)(0 - 16));
}

int rtl8139_poll(void)
{
    if (!g_card.present) return 0;

    uint16_t isr = get16(g_card.io_base, REG_INTRSTAT);
    if (isr) put16(g_card.io_base, REG_INTRSTAT, isr);

    int delivered = 0;

    while ((get8(g_card.io_base, REG_CHIPCMD) & CMD_BUFE) == 0) {
        uint8_t *hdr = g_card.rx_buf + g_rx_cursor;
        uint16_t status = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        uint16_t total_len = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));

        if (total_len == 0xFFFF) break; /* NIC is still DMA'ing this one in, come back later */

        if (!(status & RX_OK) || total_len < 4 || total_len > 1600) {
            recover_rx_ring(g_card.io_base);
            break;
        }

        uint16_t payload_len = (uint16_t)(total_len - 4);
        if (g_rx_cb) g_rx_cb(hdr + 4, payload_len);
        delivered++;

        /* skip the 4-byte header plus the packet, round up to a 4-byte
         * boundary (the chip requires it), and wrap at the ring size. */
        g_rx_cursor = (uint32_t)((g_rx_cursor + total_len + 4 + 3) & ~3u);
        g_rx_cursor %= RX_RING_BYTES;
        put16(g_card.io_base, REG_RXBUFTAIL, (uint16_t)(g_rx_cursor - 16));
    }

    return delivered;
}
