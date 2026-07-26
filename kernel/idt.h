#ifndef PEFIA_IDT_H
#define PEFIA_IDT_H

#include <stdint.h>

/* Register snapshot the assembly stubs build on the stack before calling into
 * C. Layout must match the push order in idt_asm.asm. */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

void     idt_init(void);        /* build + load the IDT, remap the PIC, start the PIT */
uint32_t timer_ticks(void);     /* ticks since boot (PIT runs at TIMER_HZ) */

#define TIMER_HZ 100

#endif
