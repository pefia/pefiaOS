#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "clock.h"

/* ---- register offsets (MMIO BAR0) ---- */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_SLU    (1u << 6)
#define CTRL_ASDE   (1u << 5)
#define CTRL_RST    (1u << 26)
#define CTRL_PHYRST (1u << 31)
#define CTRL_ILOS   (1u << 7)
#define CTRL_LRST   (1u << 3)

#define RCTL_EN     (1u << 1)
#define RCTL_UPE    (1u << 3)
#define RCTL_MPE    (1u << 4)
#define RCTL_BAM    (1u << 15)
#define RCTL_SECRC  (1u << 26)
/* BSIZE 2048 == 00, no BSEX */

#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)

#define TXD_EOP     (1u << 0)
#define TXD_IFCS    (1u << 1)
#define TXD_RS      (1u << 3)
#define TXD_STA_DD  (1u << 0)

#define RXD_STA_DD  (1u << 0)
#define RXD_STA_EOP (1u << 1)

#define NUM_RX 32
#define NUM_TX 16
#define BUF_SZ 2048

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

static volatile uint8_t *g_mmio = 0;
static int       g_present = 0;
static uint8_t   g_mac[6];
static nic_rx_cb_t g_cb = 0;

static rx_desc_t *g_rx;
static tx_desc_t *g_tx;
static uint8_t   *g_rxbuf;
static uint8_t   *g_txbuf;
static uint32_t   g_rx_cur, g_tx_cur;

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)(g_mmio + reg); }
static inline void     wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(g_mmio + reg) = v; }

static void pci_enable(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t cmd = pci_config_read16(bus, slot, func, 0x04);
    cmd |= 0x0006;   /* memory space + bus master */
    outl(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)slot<<11) | ((uint32_t)func<<8) | 0x04);
    outl(0xCFC, (uint32_t)cmd);
}

static void *alloc_aligned(int bytes, int align)
{
    uint8_t *p = (uint8_t *)kmalloc(bytes + align);
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p;
    a = (a + (align - 1)) & ~((uintptr_t)align - 1);
    return (void *)a;
}

static int eeprom_mac(uint8_t mac[6])
{
    for (int w = 0; w < 3; w++) {
        wr(REG_EERD, ((uint32_t)w << 8) | 1u);
        uint32_t v = 0; int ok = 0;
        for (int i = 0; i < 200000; i++) { v = rd(REG_EERD); if (v & (1u << 4)) { ok = 1; break; } }
        if (!ok) return -1;
        uint16_t data = (uint16_t)(v >> 16);
        mac[w*2]   = (uint8_t)data;
        mac[w*2+1] = (uint8_t)(data >> 8);
    }
    return 0;
}

static const uint16_t E1000_IDS[] = {
    0x100E, 0x100F, 0x1004, 0x1010, 0x1012, 0x1015, 0x1016, 0x1017,
    0x1019, 0x101A, 0x1026, 0x1027, 0x1028, 0x105E, 0x10D3, 0x10A4,
    0x107C, 0x1079, 0x108A, 0x1112, 0x1107
};

int e1000_init(void)
{
    PciDeviceInfo dev;
    int found = 0;
    for (int i = 0; i < (int)(sizeof(E1000_IDS)/sizeof(E1000_IDS[0])); i++) {
        if (pci_find_device_by_id(0x8086, E1000_IDS[i], &dev)) { found = 1; break; }
    }
    if (!found) return -1;

    pci_enable(dev.bus, dev.slot, dev.func);

    uint32_t bar0 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x10);
    if (bar0 & 0x1) return -2;                 /* need a memory BAR */
    g_mmio = (volatile uint8_t *)(uintptr_t)(bar0 & ~0xFu);

    /* reset */
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    clock_delay_ms(20);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++) { }
    wr(REG_IMC, 0xFFFFFFFF);
    (void)rd(REG_ICR);

    /* link up, auto-speed detect, no loopback/reset bits */
    uint32_t ctrl = rd(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHYRST | CTRL_ILOS);
    wr(REG_CTRL, ctrl);

    /* MAC: prefer the receive-address regs (loaded from EEPROM at power-on) */
    uint32_t ral = rd(REG_RAL0), rah = rd(REG_RAH0);
    if (rah & (1u << 31)) {
        g_mac[0]=(uint8_t)ral; g_mac[1]=(uint8_t)(ral>>8); g_mac[2]=(uint8_t)(ral>>16); g_mac[3]=(uint8_t)(ral>>24);
        g_mac[4]=(uint8_t)rah; g_mac[5]=(uint8_t)(rah>>8);
    } else if (eeprom_mac(g_mac) != 0) {
        return -3;
    }

    /* clear multicast table */
    for (int i = 0; i < 128; i++) wr(REG_MTA + i*4, 0);

    /* RX ring */
    g_rx = (rx_desc_t *)alloc_aligned(NUM_RX * sizeof(rx_desc_t), 16);
    g_rxbuf = (uint8_t *)alloc_aligned(NUM_RX * BUF_SZ, 16);
    if (!g_rx || !g_rxbuf) return -4;
    for (int i = 0; i < NUM_RX; i++) {
        g_rx[i].addr = (uint64_t)(uintptr_t)(g_rxbuf + i * BUF_SZ);
        g_rx[i].status = 0;
    }
    wr(REG_RDBAL, (uint32_t)(uintptr_t)g_rx);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, NUM_RX * sizeof(rx_desc_t));
    wr(REG_RDH, 0);
    wr(REG_RDT, NUM_RX - 1);
    g_rx_cur = 0;

    /* install our MAC into the receive-address filter */
    wr(REG_RAL0, (uint32_t)g_mac[0] | ((uint32_t)g_mac[1]<<8) | ((uint32_t)g_mac[2]<<16) | ((uint32_t)g_mac[3]<<24));
    wr(REG_RAH0, (uint32_t)g_mac[4] | ((uint32_t)g_mac[5]<<8) | (1u<<31));

    wr(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    /* TX ring */
    g_tx = (tx_desc_t *)alloc_aligned(NUM_TX * sizeof(tx_desc_t), 16);
    g_txbuf = (uint8_t *)alloc_aligned(NUM_TX * BUF_SZ, 16);
    if (!g_tx || !g_txbuf) return -5;
    for (int i = 0; i < NUM_TX; i++) { g_tx[i].addr = 0; g_tx[i].cmd = 0; g_tx[i].status = TXD_STA_DD; }
    wr(REG_TDBAL, (uint32_t)(uintptr_t)g_tx);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, NUM_TX * sizeof(tx_desc_t));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    g_tx_cur = 0;

    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    wr(REG_TIPG, 10u | (8u << 10) | (6u << 20));

    g_present = 1;
    return 0;
}

int e1000_present(void) { return g_present; }
const uint8_t *e1000_mac(void) { return g_mac; }
void e1000_set_rx_callback(nic_rx_cb_t cb) { g_cb = cb; }

int e1000_send(const void *frame, uint16_t len)
{
    if (!g_present) return -1;
    if (len > BUF_SZ) return -2;

    uint32_t idx = g_tx_cur;
    uint8_t *buf = g_txbuf + idx * BUF_SZ;
    const uint8_t *src = (const uint8_t *)frame;
    for (uint16_t i = 0; i < len; i++) buf[i] = src[i];

    g_tx[idx].addr = (uint64_t)(uintptr_t)buf;
    g_tx[idx].length = len;
    g_tx[idx].cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    g_tx[idx].status = 0;

    g_tx_cur = (idx + 1) % NUM_TX;
    wr(REG_TDT, g_tx_cur);

    for (int i = 0; i < 100000 && !(g_tx[idx].status & TXD_STA_DD); i++) { }
    return len;
}

int e1000_poll(void)
{
    if (!g_present) return 0;
    int delivered = 0;
    while (g_rx[g_rx_cur].status & RXD_STA_DD) {
        uint16_t len = g_rx[g_rx_cur].length;       /* CRC already stripped (SECRC) */
        uint8_t *buf = g_rxbuf + g_rx_cur * BUF_SZ;
        if ((g_rx[g_rx_cur].status & RXD_STA_EOP) && len >= 14 && len <= BUF_SZ) {
            if (g_cb) g_cb(buf, len);
            delivered++;
        }
        g_rx[g_rx_cur].status = 0;
        uint32_t old = g_rx_cur;
        g_rx_cur = (g_rx_cur + 1) % NUM_RX;
        wr(REG_RDT, old);
    }
    return delivered;
}
