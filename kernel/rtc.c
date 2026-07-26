#include "rtc.h"
#include "io.h"
#include <stdint.h>

static uint8_t cmos_read(int reg)
{
    outb(0x70, (uint8_t)reg);
    return inb(0x71);
}

static void cmos_write(int reg, uint8_t val)
{
    outb(0x70, (uint8_t)reg);
    outb(0x71, val);
}

/* Wait out the RTC's update-in-progress flag (status A bit 7) so we never
 * touch the time registers mid-rollover. Bounded: the window only lasts a
 * couple of ms, so if the bit never drops the chip is broken and we just
 * carry on with whatever it gives us. */
static void wait_update_done(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(cmos_read(0x0A) & 0x80)) return;
    }
}

/* Write seconds/minutes/hours back in whatever encoding rtc_time() reads.
 * Waits for update-in-progress to clear first so the writes can't land
 * mid-rollover. ponytail: writes 24h binary/BCD to regs 0/2/4 only; still
 * ignores the 12h-mode PM bit. Good enough for "nudge the clock" in
 * Settings; upgrade if a real time daemon ever needs it. */
void rtc_set(int h, int m, int s)
{
    uint8_t status_b = cmos_read(0x0B);
    int is_binary = (status_b & 0x04);

    if (h < 0) h = 0;
    if (h > 23) h = 23;
    if (m < 0) m = 0;
    if (m > 59) m = 59;
    if (s < 0) s = 0;
    if (s > 59) s = 59;

    uint8_t sec  = is_binary ? (uint8_t)s : (uint8_t)(((s / 10) << 4) | (s % 10));
    uint8_t min  = is_binary ? (uint8_t)m : (uint8_t)(((m / 10) << 4) | (m % 10));
    uint8_t hour = is_binary ? (uint8_t)h : (uint8_t)(((h / 10) << 4) | (h % 10));

    wait_update_done();
    cmos_write(0x00, sec);
    cmos_write(0x02, min);
    cmos_write(0x04, hour);
}

void rtc_time(int *h, int *m, int *s)
{
    uint8_t sec, min, hour;

    /* A read landing inside the chip's once-a-second update window hands
     * back torn values, and clock.c calibrates the TSC off our seconds
     * field - one bad read there skews every timeout in the OS until
     * reboot. So: wait out the update, then re-read until two consecutive
     * passes agree. */
    do {
        wait_update_done();
        sec  = cmos_read(0x00);
        min  = cmos_read(0x02);
        hour = cmos_read(0x04);
    } while (sec != cmos_read(0x00) || min != cmos_read(0x02) || hour != cmos_read(0x04));

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
