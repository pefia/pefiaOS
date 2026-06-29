#include "clock.h"
#include "rtc.h"

static uint64_t g_cycles_per_ms = 0;
static uint64_t g_base_tsc      = 0;
static int      g_ready         = 0;

static int rtc_seconds(void)
{
    int h, m, s;
    rtc_time(&h, &m, &s);
    return h * 3600 + m * 60 + s;
}

void clock_init(void)
{
    if (g_ready) return;

    /* Wait for the RTC second to tick over, then count TSC cycles across one
     * full second. This gives cycles-per-second without any timer hardware. */
    int s0 = rtc_seconds();
    int s1;
    do { s1 = rtc_seconds(); } while (s1 == s0);

    uint64_t t1 = rdtsc();
    int s2;
    do { s2 = rtc_seconds(); } while (s2 == s1);
    uint64_t t2 = rdtsc();

    uint64_t cps = t2 - t1;
    if (cps < 1000) cps = 1000000000ULL;  /* implausible -> assume ~1GHz */
    g_cycles_per_ms = cps / 1000;
    if (g_cycles_per_ms == 0) g_cycles_per_ms = 1;

    g_base_tsc = rdtsc();
    g_ready    = 1;
}

uint32_t clock_ms(void)
{
    if (!g_ready) return 0;
    uint64_t d = rdtsc() - g_base_tsc;
    return (uint32_t)(d / g_cycles_per_ms);
}

void clock_delay_ms(uint32_t ms)
{
    uint32_t start = clock_ms();
    while ((uint32_t)(clock_ms() - start) < ms) {
        __asm__ volatile ("pause");
    }
}
