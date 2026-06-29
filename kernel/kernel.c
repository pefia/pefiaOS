/* kernel/kernel.c
 * -----------------------------------------------------------------------------
 * C entry point. Brings up the framebuffer, heap, input, mouse and the
 * in-memory filesystem, then hands control to the window manager + desktop.
 * -----------------------------------------------------------------------------
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

uint32_t g_mem_kb = 0;   /* defined here, declared in sysinfo.h */

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
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

    /* Open the browser on its (instant, local) start page so the machine is
     * immediately useful; the user can type a URL to go online. */
    wm_create_browser(70, 50, 880, 660);

    wm_run();   /* never returns */
}
