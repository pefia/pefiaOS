/* CMOS RTC access via ports 0x70/0x71. No interrupts, no periodic update
 * IRQ - the taskbar just polls this each time it redraws the clock, which
 * is cheap enough that nobody's bothered wiring up anything smarter. */
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
    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t status_b = cmos_read(0x0B);

    if (!(status_b & 0x04)) {   /* bit2 clear means the registers are BCD, not binary */
        sec  = (uint8_t)((sec & 0x0F) + ((sec >> 4) * 10));
        min  = (uint8_t)((min & 0x0F) + ((min >> 4) * 10));
        hour = (uint8_t)(((hour & 0x0F) + (((hour & 0x70) >> 4) * 10)) | (hour & 0x80));
    }
    if (!(status_b & 0x02) && (hour & 0x80)) {   /* 12h mode with the PM flag set */
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    }

    *s = sec; *m = min; *h = hour;
}
