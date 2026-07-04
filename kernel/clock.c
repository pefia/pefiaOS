#include "clock.h"
#include "rtc.h"

static uint64_t tsc_cycles_per_ms = 0;
static uint64_t tsc_base          = 0;
static int      calibrated        = 0;

static int rtc_seconds(void)
{
    int h, m, s;
    rtc_time(&h, &m, &s);
    return h * 3600 + m * 60 + s;
}

void clock_init(void)
{
    if (calibrated) return;

    /* No timer hardware means no interrupt to count cycles-per-tick
     * against, so instead we wait for the RTC's seconds field to roll
     * over and count TSC cycles across exactly one full second of it. */
    int prev = rtc_seconds();
    int cur;
    do { cur = rtc_seconds(); } while (cur == prev);

    uint64_t t_start = rdtsc();
    int next;
    do { next = rtc_seconds(); } while (next == cur);
    uint64_t t_end = rdtsc();

    uint64_t cycles_per_sec = t_end - t_start;
    if (cycles_per_sec < 1000) cycles_per_sec = 1000000000ULL;  /* nonsense reading, assume ~1GHz */
    tsc_cycles_per_ms = cycles_per_sec / 1000;
    if (tsc_cycles_per_ms == 0) tsc_cycles_per_ms = 1;

    tsc_base   = rdtsc();
    calibrated = 1;
}

uint32_t clock_ms(void)
{
    if (!calibrated) return 0;
    uint64_t elapsed = rdtsc() - tsc_base;
    return (uint32_t)(elapsed / tsc_cycles_per_ms);
}

void clock_delay_ms(uint32_t ms)
{
    uint32_t start = clock_ms();
    while ((uint32_t)(clock_ms() - start) < ms) {
        __asm__ volatile ("pause");
    }
}
