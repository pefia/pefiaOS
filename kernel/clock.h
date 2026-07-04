/* Millisecond time source built on the CPU's TSC, calibrated once against
 * the CMOS RTC at boot. pefiaOS has no PIT or APIC timer wired up, so
 * anything that needs coarse timing - the network stack's retransmit
 * pacing, mainly - ends up here instead. */
#ifndef PEFIA_CLOCK_H
#define PEFIA_CLOCK_H

#include <stdint.h>

/* Measures TSC frequency against the RTC. Blocks up to ~2 seconds. Safe
 * to call more than once - only the first call actually calibrates. */
void     clock_init(void);

/* Milliseconds elapsed since clock_init(). Monotonic. */
uint32_t clock_ms(void);

/* Busy-wait for the given number of milliseconds. */
void     clock_delay_ms(uint32_t ms);

/* Raw timestamp counter. */
static inline uint64_t rdtsc(void)
{
    uint64_t v;
    __asm__ volatile ("rdtsc" : "=A"(v));
    return v;
}

#endif /* PEFIA_CLOCK_H */
