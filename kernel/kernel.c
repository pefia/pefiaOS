/* kernel/kernel.c
 * C entry point, called from _start in boot/boot.asm. Brings the machine up
 * one subsystem at a time and then hands off to the window manager for good.
 */
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

uint32_t g_mem_kb = 0;   /* declared extern in sysinfo.h, lives here */

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    /* No framebuffer, no desktop - nothing else is worth bringing up. */
    if (fb_init(magic, mb_info_addr) != 0) {
        for (;;) __asm__ volatile ("hlt");
    }

    struct multiboot_info *mb = (struct multiboot_info *)mb_info_addr;
    if (mb->flags & MULTIBOOT_INFO_MEMORY)
        g_mem_kb = mb->mem_lower + mb->mem_upper;

    heap_init_from_multiboot(mb);

    input_init();
    mouse_cursor_init(fb_width() / 2, fb_height() / 2);

    vfs_init();
    net_init();
    wm_init();

    /* Land on the browser's local start page rather than a bare desktop -
     * it's instant (no network needed) and gives the user something to do
     * immediately; typing a URL takes it online. */
    wm_create_browser(70, 50, 880, 660);

    wm_run();   /* does not return */
}
