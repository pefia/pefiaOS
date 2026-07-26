#include "framebuffer.h"
#include "input.h"
#include "mouse.h"
#include "wm.h"
#include "heap.h"
#include "vfs.h"
#include "multiboot.h"
#include "sysinfo.h"
#include "net.h"
#include "browser.h"
#include "gdt.h"
#include "idt.h"
#include "sched.h"
#include "sound.h"
#include "interp.h"
#include "ata.h"
#include "util.h"

uint32_t g_mem_kb = 0;   /* declared extern in sysinfo.h, lives here */

/* Boot self-test: exercises the expression interpreter, the writable VFS, and
 * the scheduler/timer, then pops the results into a desktop window. Doubles as
 * a runnable check that these subsystems actually work - if any line reads FAIL,
 * something regressed. */
static void boot_selftest(void)
{
    int ok1 = 0, ok2 = 0, ok3 = 0;
    long a = interp_eval("84/2", &ok1);
    long b = interp_eval("(2+3)*4-8", &ok2);
    long c = interp_eval("100%7", &ok3);
    int interp_ok = ok1 && ok2 && ok3 && a == 42 && b == 12 && c == 2;

    int vfs_ok = 0;
    int idx = vfs_create(vfs_root(), "selftest.tmp", 0);
    if (idx >= 0 && vfs_write(idx, "hi", 2) == 0) {
        const VNode *v = vfs_node(idx);
        vfs_ok = (v && v->content && v->content[0] == 'h' && v->content[1] == 'i' && v->size == 2);
    }
    if (idx >= 0) vfs_delete(idx);

    /* ATA disk: read sector 0 if a drive answered, show its first bytes. */
    char disk[16]; kstrcpy(disk, "none");
    if (ata_present()) {
        static uint8_t sec[512];
        if (ata_read(0, 1, sec) == 0) {
            int p = 0;
            for (int i = 0; i < 4; i++) disk[p++] = (sec[i] >= 32 && sec[i] < 127) ? (char)sec[i] : '.';
            disk[p] = '\0';
        } else {
            kstrcpy(disk, "read-err");
        }
    }

    char body[96], num[11];
    kstrcpy(body, "interp 84/2=");
    kutoa((uint32_t)a, num);            kstrcat(body, num);
    kstrcat(body, interp_ok ? " OK " : " FAIL ");
    kstrcat(body, "VFS ");              kstrcat(body, vfs_ok ? "rw OK " : "FAIL ");
    kstrcat(body, "thr=");
    kutoa((uint32_t)sched_count(), num); kstrcat(body, num);
    kstrcat(body, " tk=");
    kutoa(timer_ticks(), num);          kstrcat(body, num);
    kstrcat(body, " disk[0..3]=");      kstrcat(body, disk);

    wm_create_info(90, 90, 700, 150, "pefiaOS self-test", body);
}

/* A demo background kernel thread: it just yields in a loop so `ps` in the
 * terminal shows a second live thread alongside the WM. Proof the scheduler
 * and context switch actually work; real work would replace the body. */
static void worker_thread(void)
{
    for (;;) sched_yield();
}

/* Last-resort diagnostics through the VGA text buffer at 0xB8000.
 *
 * This is the one path that runs when fb_init fails, which means the desktop
 * can never appear - and a kernel that just halts looks identical to a
 * kernel that crashed. The usual cause is booting the ISO under UEFI
 * firmware (the rescue image is BIOS/CSM only) or a machine whose firmware
 * granted no linear framebuffer, so say that rather than showing a black
 * screen. Text mode is a safe assumption here precisely because graphics
 * mode is what we failed to get. */
static void text_panic(const char *why)
{
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    static const char *lines[] = {
        "pefiaOS could not start: no linear framebuffer.",
        "",
        "Most likely the VM is using UEFI firmware. This ISO boots via BIOS",
        "(legacy/CSM). Switch the VM firmware to BIOS and boot it again.",
        "",
        "Machine type does not matter: q35/ICH9 and the older i440FX both",
        "work, and a 64-bit VM CPU is fine - the kernel is 32-bit but runs",
        "on any x86_64 host CPU.",
        0
    };

    for (int i = 0; i < 80 * 25; i++) vga[i] = (uint16_t)(0x1F00 | ' ');

    int row = 2;
    for (int l = 0; lines[l]; l++, row++)
        for (int c = 0; lines[l][c] && c < 78; c++)
            vga[row * 80 + c + 1] = (uint16_t)(0x1F00 | (unsigned char)lines[l][c]);

    row++;
    for (int c = 0; why[c] && c < 78; c++)
        vga[row * 80 + c + 1] = (uint16_t)(0x1E00 | (unsigned char)why[c]);
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    /* No framebuffer, no desktop - nothing else is worth bringing up. */
    if (fb_init(magic, mb_info_addr) != 0) {
        const char *why = "detail: bootloader did not hand over a Multiboot RGB framebuffer.";
        if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
            why = "detail: not booted by a Multiboot loader (bad magic in eax).";
        text_panic(why);
        for (;;) __asm__ volatile ("hlt");
    }

    struct multiboot_info *mb = (struct multiboot_info *)mb_info_addr;
    if (mb->flags & MULTIBOOT_INFO_MEMORY)
        g_mem_kb = mb->mem_lower + mb->mem_upper;

    heap_init_from_multiboot(mb);

    gdt_init();          /* our own flat GDT so selectors 0x08/0x10 are ours to trust */
    idt_init();          /* IDT + PIC remap + PIT timer IRQ (IRQ0 only) */
    sched_init();        /* register this boot context as thread 0 */

    input_init();
    mouse_cursor_init(fb_width() / 2, fb_height() / 2);

    vfs_init();
    net_init();
    sound_init();
    wm_init();

    sched_spawn("worker", worker_thread);

    /* Land on the browser's local start page rather than a bare desktop -
     * it's instant (no network needed) and gives the user something to do
     * immediately; typing a URL takes it online. */
    wm_create_browser(70, 50, 880, 660);

    boot_selftest();   /* opens a results window on top of the browser */

    wm_run();
}
