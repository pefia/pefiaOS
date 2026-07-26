#include "idt.h"
#include "io.h"
#include "console.h"
#include "framebuffer.h"
#include "sched.h"
#include "util.h"

#define IDT_ENTRIES 256

struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

/* The assembly stubs, one per vector we care about (idt_asm.asm). */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);
extern void idt_flush(uint32_t);

static void set_gate(int n, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[n].base_lo = base & 0xFFFF;
    idt[n].base_hi = (base >> 16) & 0xFFFF;
    idt[n].sel     = sel;
    idt[n].always0 = 0;
    idt[n].flags   = flags;   /* 0x8E = present, ring 0, 32-bit interrupt gate */
}

/* Move both PICs' vector bases: master to 0x20, slave to 0x28. Standard ICW
 * init sequence, cascaded on IRQ2. */
static void pic_remap(void)
{
    uint8_t mask1 = inb(0x21), mask2 = inb(0xA1);

    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();   /* tell master slave is at IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    outb(0x21, mask1);
    outb(0xA1, mask2);
}

static void pit_init(uint32_t hz)
{
    uint32_t divisor = 1193182u / hz;
    outb(0x43, 0x36);                          /* channel 0, lo/hi, mode 3 (square wave) */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void idt_init(void)
{
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    kmemset(&idt, 0, sizeof(idt));

    pic_remap();

    void (*stubs[48])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9,
        isr10, isr11, isr12, isr13, isr14, isr15, isr16, isr17, isr18, isr19,
        isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29,
        isr30, isr31,
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15,
    };
    for (int i = 0; i < 48; i++)
        set_gate(i, (uint32_t)stubs[i], 0x08, 0x8E);

    idt_flush((uint32_t)&idtp);

    pit_init(TIMER_HZ);

    /* Unmask IRQ0 (timer) on the master, leave everything else masked - the
     * keyboard and mouse are still serviced by polling. Bit set = masked. */
    outb(0x21, 0xFE);
    outb(0xA1, 0xFF);

    __asm__ volatile ("sti");
}

/* --- C dispatch, called from the common stubs with a pointer to regs_t --- */

static volatile uint32_t ticks = 0;

uint32_t timer_ticks(void) { return ticks; }

static const char *EXC_NAME[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "BOUND range",
    "invalid opcode", "device N/A", "double fault", "coproc overrun",
    "invalid TSS", "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 FP", "alignment", "machine check", "SIMD FP",
    "virt", "control-protection", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
};

/* A CPU exception is not something this kernel can recover from, so print what
 * we know and stop rather than triple-faulting into a reboot loop. */
static void panic_exception(regs_t *r)
{
    color_t bg = fb_rgb(20, 0, 40), fg = fb_rgb(255, 220, 220);
    fb_fill_rect(0, 0, fb_width(), 48, bg);
    char num[11];
    gfx_text(8, 6,  "KERNEL EXCEPTION - halted", fg, bg);
    const char *nm = (r->int_no < 32) ? EXC_NAME[r->int_no] : "?";
    gfx_text(8, 24, nm, fg, bg);
    kutoa(r->int_no, num); gfx_text(320, 24, "vec=", fg, bg); gfx_text(360, 24, num, fg, bg);
    kutoa(r->eip, num);    gfx_text(430, 24, "eip=", fg, bg); gfx_text(470, 24, num, fg, bg);
    for (;;) __asm__ volatile ("cli; hlt");
}

void isr_handler(regs_t *r)
{
    if (r->int_no < 32) panic_exception(r);
}

void irq_handler(regs_t *r)
{
    int irq = r->int_no - 32;

    if (irq == 0) {
        ticks++;
        sched_tick();     /* cooperative: records a tick, does not force a switch */
    }

    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
