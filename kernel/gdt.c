#include "gdt.h"
#include "util.h"

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 32-bit TSS. We only really populate ss0/esp0 (the ring-0 stack the CPU
 * switches to on a ring3->ring0 trap) and iomap_base. */
struct tss_entry {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr   gp;
static struct tss_entry tss;

extern void gdt_flush(uint32_t);
extern void tss_flush(void);

static void set_entry(int n, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[n].base_lo  = base & 0xFFFF;
    gdt[n].base_mid = (base >> 16) & 0xFF;
    gdt[n].base_hi  = (base >> 24) & 0xFF;
    gdt[n].limit_lo = limit & 0xFFFF;
    gdt[n].gran     = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[n].access   = access;
}

void gdt_init(void)
{
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)&gdt;

    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);
    set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);

    kmemset(&tss, 0, sizeof(tss));
    tss.ss0        = KDATA_SEL;
    tss.esp0       = 0;
    tss.iomap_base = sizeof(tss);
    uint32_t base = (uint32_t)&tss, limit = sizeof(tss) - 1;
    set_entry(5, base, limit, 0x89, 0x00);

    gdt_flush((uint32_t)&gp);
    tss_flush();
}

void tss_set_stack(uint32_t esp0) { tss.esp0 = esp0; }
