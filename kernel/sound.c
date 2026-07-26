#include "sound.h"
#include "io.h"
#include "clock.h"

void sound_tone(uint32_t hz)
{
    if (hz == 0) { sound_off(); return; }

    uint32_t divisor = 1193182u / hz;
    outb(0x43, 0xB6);                              /* channel 2, lo/hi byte, mode 3 */
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t gate = inb(0x61);
    if ((gate & 3) != 3) outb(0x61, gate | 3);
}

void sound_off(void)
{
    outb(0x61, inb(0x61) & 0xFC);
}

void sound_beep(uint32_t hz, uint32_t ms)
{
    sound_tone(hz);
    clock_delay_ms(ms);
    sound_off();
}

void sound_init(void)
{
    sound_off();
    /* A two-note boot chime - also an audible sign interrupts/timing are up.
     * clock_init() has already run by now (net_init calls it), so the delays
     * are real milliseconds. */
    sound_beep(660, 120);
    sound_beep(990, 160);
}
