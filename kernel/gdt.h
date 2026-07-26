#ifndef PEFIA_GDT_H
#define PEFIA_GDT_H

#include <stdint.h>

#define KCODE_SEL 0x08
#define KDATA_SEL 0x10
#define UCODE_SEL 0x1B
#define UDATA_SEL 0x23
#define TSS_SEL   0x28

void gdt_init(void);
void tss_set_stack(uint32_t esp0);   /* kernel stack to use on a ring3->ring0 trap */

#endif
