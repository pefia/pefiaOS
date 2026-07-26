#ifndef PEFIA_SOUND_H
#define PEFIA_SOUND_H

#include <stdint.h>

void sound_init(void);                       /* silence the speaker, play a short boot chime */
void sound_tone(uint32_t hz);                /* start a continuous tone (0 = stop) */
void sound_off(void);
void sound_beep(uint32_t hz, uint32_t ms);   /* tone for ms, then silence (blocks) */

#endif
