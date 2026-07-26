#ifndef PEFIA_ATA_H
#define PEFIA_ATA_H

#include <stdint.h>

int ata_present(void);                                   /* 1 if a master drive answered */
int ata_read(uint32_t lba, uint8_t count, void *buf);
int ata_write(uint32_t lba, uint8_t count, const void *buf);

#endif
