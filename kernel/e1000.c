#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "clock.h"
#include "util.h"

/* register offsets into the MMIO BAR - see the Intel SDM for 8254x-family
 * NICs, section on device registers. only the subset we actually touch. */
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
#define RCTL_SECRC  (1u << 26)   /* strip the Ethernet CRC before it hits our buffer */
/* buffer size field left at 0 => 2048-byte receive buffers, which is BUF_SZ below */

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

/* legacy (non-extended) descriptor formats - fixed by the hardware, don't
 * reorder these fields. */
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

static volatile uint8_t *nic_mmio = 0;
static int          nic_up = 0;
static uint8_t       nic_mac_addr[6];
static nic_rx_cb_t   rx_callback = 0;

static rx_desc_t *rx_ring;
static tx_desc_t *tx_ring;
static uint8_t   *rx_bufs;
static uint8_t   *tx_bufs;
static uint32_t   rx_tail_seen, tx_next_slot;

static inline uint32_t reg_read(uint32_t off)  { return *(volatile uint32_t *)(nic_mmio + off); }
static inline void     reg_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(nic_mmio + off) = v; }

/* Spin on a register until (value & mask) is nonzero, or give up after
 * `tries` reads. Returns 1 if it showed up, 0 if we timed out - the eeprom
 * read and the reset-bit wait both need exactly this. */
static int wait_for_bit(uint32_t off, uint32_t mask, int tries)
{
    for (int i = 0; i < tries; i++) {
        if (reg_read(off) & mask) return 1;
    }
    return 0;
}

static void pci_enable_mem_and_master(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t cmd = pci_config_read16(bus, slot, func, 0x04);
    cmd |= 0x0006;
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | 0x04);
    outl(0xCFC, (uint32_t)cmd);
}

/* kmalloc gives us no alignment guarantee, and both descriptor rings need
 * a 16-byte aligned base per the datasheet, so overallocate and shift. */
static void *alloc_aligned(uint32_t bytes, uint32_t align)
{
    uint8_t *raw = (uint8_t *)kmalloc(bytes + align);
    if (!raw) return 0;
    uintptr_t addr = (uintptr_t)raw;
    addr = (addr + (align - 1)) & ~((uintptr_t)align - 1);
    return (void *)addr;
}

/* Fallback path for cards that don't expose the MAC through RAL/RAH at
 * reset (some virtualized ones do, some don't) - pull it straight out of
 * the EEPROM three words at a time. */
static int read_mac_from_eeprom(uint8_t mac[6])
{
    for (int word = 0; word < 3; word++) {
        reg_write(REG_EERD, ((uint32_t)word << 8) | 1u);
        if (!wait_for_bit(REG_EERD, 1u << 4, 200000)) return -1;
        uint16_t data = (uint16_t)(reg_read(REG_EERD) >> 16);
        mac[word * 2]     = (uint8_t)data;
        mac[word * 2 + 1] = (uint8_t)(data >> 8);
    }
    return 0;
}

/* PCI device IDs for the 8254x family we know how to drive. Nowhere near
 * exhaustive, but covers what QEMU/VirtualBox/real hardware we've tested
 * against actually report. */
static const uint16_t KNOWN_E1000_IDS[] = {
    0x100E, 0x100F, 0x1004, 0x1010, 0x1012, 0x1015, 0x1016, 0x1017,
    0x1019, 0x101A, 0x1026, 0x1027, 0x1028, 0x105E, 0x10D3, 0x10A4,
    0x107C, 0x1079, 0x108A, 0x1112, 0x1107
};
#define NUM_KNOWN_IDS (int)(sizeof(KNOWN_E1000_IDS) / sizeof(KNOWN_E1000_IDS[0]))

static int find_e1000(PciDeviceInfo *out)
{
    for (int i = 0; i < NUM_KNOWN_IDS; i++) {
        if (pci_find_device_by_id(0x8086, KNOWN_E1000_IDS[i], out)) return 1;
    }
    return 0;
}

static void reset_and_bring_up_link(void)
{
    reg_write(REG_IMC, 0xFFFFFFFF); /* mask everything, we poll instead of using IRQs */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    clock_delay_ms(20);
    for (int i = 0; i < 100000 && (reg_read(REG_CTRL) & CTRL_RST); i++) { }
    reg_write(REG_IMC, 0xFFFFFFFF);
    (void)reg_read(REG_ICR); /* reading ICR clears it - discard whatever piled up during reset */

    uint32_t ctrl = reg_read(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHYRST | CTRL_ILOS);
    reg_write(REG_CTRL, ctrl);
}

static int resolve_mac_address(void)
{
    uint32_t ral = reg_read(REG_RAL0), rah = reg_read(REG_RAH0);
    if (rah & (1u << 31)) {
        nic_mac_addr[0] = (uint8_t)ral;
        nic_mac_addr[1] = (uint8_t)(ral >> 8);
        nic_mac_addr[2] = (uint8_t)(ral >> 16);
        nic_mac_addr[3] = (uint8_t)(ral >> 24);
        nic_mac_addr[4] = (uint8_t)rah;
        nic_mac_addr[5] = (uint8_t)(rah >> 8);
        return 0;
    }
    return read_mac_from_eeprom(nic_mac_addr);
}

static int setup_rx_ring(void)
{
    rx_ring = (rx_desc_t *)alloc_aligned(NUM_RX * sizeof(rx_desc_t), 16);
    rx_bufs = (uint8_t *)alloc_aligned(NUM_RX * BUF_SZ, 16);
    if (!rx_ring || !rx_bufs) return -1;

    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)(rx_bufs + i * BUF_SZ);
        rx_ring[i].status = 0;
    }

    reg_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    reg_write(REG_RDBAH, 0);
    reg_write(REG_RDLEN, NUM_RX * sizeof(rx_desc_t));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, NUM_RX - 1);
    rx_tail_seen = 0;
    return 0;
}

static int setup_tx_ring(void)
{
    tx_ring = (tx_desc_t *)alloc_aligned(NUM_TX * sizeof(tx_desc_t), 16);
    tx_bufs = (uint8_t *)alloc_aligned(NUM_TX * BUF_SZ, 16);
    if (!tx_ring || !tx_bufs) return -1;

    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].addr = 0;
        tx_ring[i].cmd = 0;
        tx_ring[i].status = TXD_STA_DD;
    }

    reg_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    reg_write(REG_TDBAH, 0);
    reg_write(REG_TDLEN, NUM_TX * sizeof(tx_desc_t));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    tx_next_slot = 0;
    return 0;
}

int e1000_init(void)
{
    PciDeviceInfo dev;
    if (!find_e1000(&dev)) return -1;

    pci_enable_mem_and_master(dev.bus, dev.slot, dev.func);

    uint32_t bar0 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x10);
    if (bar0 & 0x1) return -2; /* that's an I/O BAR, not the memory one we need */
    nic_mmio = (volatile uint8_t *)(uintptr_t)(bar0 & ~0xFu);

    reset_and_bring_up_link();

    if (resolve_mac_address() != 0) return -3;

    for (int i = 0; i < 128; i++) reg_write(REG_MTA + i * 4, 0);

    if (setup_rx_ring() != 0) return -4;

    /* program our own address back into the filter now that we're sure
     * what it is (EEPROM path doesn't do this on its own) */
    reg_write(REG_RAL0, (uint32_t)nic_mac_addr[0] | ((uint32_t)nic_mac_addr[1] << 8) |
                        ((uint32_t)nic_mac_addr[2] << 16) | ((uint32_t)nic_mac_addr[3] << 24));
    reg_write(REG_RAH0, (uint32_t)nic_mac_addr[4] | ((uint32_t)nic_mac_addr[5] << 8) | (1u << 31));

    reg_write(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    if (setup_tx_ring() != 0) return -5;

    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
    reg_write(REG_TIPG, 10u | (8u << 10) | (6u << 20));

    nic_up = 1;
    return 0;
}

int e1000_present(void) { return nic_up; }
const uint8_t *e1000_mac(void) { return nic_mac_addr; }
void e1000_set_rx_callback(nic_rx_cb_t cb) { rx_callback = cb; }

int e1000_send(const void *frame, uint16_t len)
{
    if (!nic_up) return -1;
    if (len > BUF_SZ) return -2;

    uint32_t slot = tx_next_slot;
    uint8_t *dst = tx_bufs + slot * BUF_SZ;
    kmemmove(dst, frame, len);

    tx_ring[slot].addr = (uint64_t)(uintptr_t)dst;
    tx_ring[slot].length = len;
    tx_ring[slot].cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    tx_ring[slot].status = 0;

    tx_next_slot = (slot + 1) % NUM_TX;
    reg_write(REG_TDT, tx_next_slot);

    /* legacy mode has no completion interrupt we listen for, so just spin
     * on the descriptor's done bit - the ring is small enough that this
     * doesn't back up in practice. */
    for (int i = 0; i < 100000 && !(tx_ring[slot].status & TXD_STA_DD); i++) { }
    return len;
}

int e1000_poll(void)
{
    if (!nic_up) return 0;

    int delivered = 0;
    while (rx_ring[rx_tail_seen].status & RXD_STA_DD) {
        uint16_t len = rx_ring[rx_tail_seen].length;
        uint8_t *frame = rx_bufs + rx_tail_seen * BUF_SZ;

        if ((rx_ring[rx_tail_seen].status & RXD_STA_EOP) && len >= 14 && len <= BUF_SZ) {
            if (rx_callback) rx_callback(frame, len);
            delivered++;
        }

        rx_ring[rx_tail_seen].status = 0;
        uint32_t completed_slot = rx_tail_seen;
        rx_tail_seen = (rx_tail_seen + 1) % NUM_RX;
        reg_write(REG_RDT, completed_slot);
    }
    return delivered;
}
