/* kernel/rtc.c
 * Reads hours/minutes/seconds from the CMOS RTC (ports 0x70/0x71). No
 * interrupts needed - we just poll it whenever the taskbar repaints.
 */
#include "rtc.h"
#include "io.h"
#include <stdint.h>

static uint8_t cmos_read(int reg)
{
    outb(0x70, (uint8_t)reg);
    return inb(0x71);
}

void rtc_time(int *h, int *m, int *s)
{
    uint8_t ss = cmos_read(0x00);
    uint8_t mm = cmos_read(0x02);
    uint8_t hh = cmos_read(0x04);
    uint8_t st = cmos_read(0x0B);     /* status register B */

    if (!(st & 0x04)) {               /* bit2 clear => values are BCD, convert */
        ss = (uint8_t)((ss & 0x0F) + ((ss >> 4) * 10));
        mm = (uint8_t)((mm & 0x0F) + ((mm >> 4) * 10));
        hh = (uint8_t)(((hh & 0x0F) + (((hh & 0x70) >> 4) * 10)) | (hh & 0x80));
    }
    if (!(st & 0x02) && (hh & 0x80)) {/* 12-hour PM flag set => make it 24h */
        hh = (uint8_t)(((hh & 0x7F) + 12) % 24);
    }

    *s = ss; *m = mm; *h = hh;
}
