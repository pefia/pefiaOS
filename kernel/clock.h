/* kernel/clock.h
 * -----------------------------------------------------------------------------
 * A coarse millisecond time source built on the CPU timestamp counter (rdtsc),
 * calibrated once against the CMOS real-time clock. There is no PIT/APIC timer
 * wired up in pefiaOS, so the network stack uses this for its timeouts and
 * retransmission pacing.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_CLOCK_H
#define PEFIA_CLOCK_H

#include <stdint.h>

/* Measure the TSC frequency against the RTC. Blocks up to ~2 seconds. Safe to
 * call more than once (only the first call performs the calibration). */
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
