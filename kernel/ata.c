#include "ata.h"
#include "io.h"

#define ATA_DATA    0x1F0
#define ATA_ERR     0x1F1
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ  0x20
#define CMD_WRITE 0x30
#define CMD_FLUSH 0xE7

/* Spin until BSY clears, bounded. Returns 0 on success, -1 on timeout. */
static int wait_not_busy(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s == 0xFF) return -1;
        if (!(s & ST_BSY)) return 0;
    }
    return -1;
}

/* Wait for DRQ (data request) with BSY clear. Returns 0, or -1 on error/timeout. */
static int wait_drq(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return 0;
    }
    return -1;
}

static void select_lba(uint32_t lba, uint8_t count)
{
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCNT, count);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_present(void)
{
    outb(ATA_DRIVE, 0xE0);
    for (int i = 0; i < 4; i++) inb(ATA_STATUS);
    uint8_t s = inb(ATA_STATUS);
    return (s != 0xFF && s != 0x00);
}

int ata_read(uint32_t lba, uint8_t count, void *buf)
{
    if (count == 0) return -1;
    if (wait_not_busy() != 0) return -1;

    select_lba(lba, count);
    outb(ATA_CMD, CMD_READ);

    uint16_t *out = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq() != 0) return -1;
        for (int w = 0; w < 256; w++) *out++ = inw(ATA_DATA);
    }
    return 0;
}

int ata_write(uint32_t lba, uint8_t count, const void *buf)
{
    if (count == 0) return -1;
    if (wait_not_busy() != 0) return -1;

    select_lba(lba, count);
    outb(ATA_CMD, CMD_WRITE);

    const uint16_t *in = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq() != 0) return -1;
        for (int w = 0; w < 256; w++) outw(ATA_DATA, *in++);
    }
    outb(ATA_CMD, CMD_FLUSH);
    wait_not_busy();
    return 0;
}
