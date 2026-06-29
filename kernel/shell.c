/* kernel/shell.c
 * -----------------------------------------------------------------------------
 * Minimal command shell for pefiaOS, rendered on the graphical console.
 * Built-ins: help, about (neofetch-style), memtest, clear, echo.
 * -----------------------------------------------------------------------------
 */
#include "shell.h"
#include "console.h"
#include "framebuffer.h"
#include "input.h"
#include "heap.h"
#include "sysinfo.h"
#include "util.h"

#include <stddef.h>

#define LINE_MAX 128

/* --- string helpers --- */

static int str_eq(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static const char *str_after_prefix(const char *s, const char *prefix)
{
    size_t i = 0;
    while (prefix[i]) { if (s[i] != prefix[i]) return NULL; i++; }
    return s + i;
}

/* --- neofetch-style `about` --- */

static color_t a_logo, a_key, a_val, a_bg;
static int a_row;

static const char *LOGO[] = {
    "        ____         ",
    "      /\\    \\\\       ",
    "     /  \\____\\\\      ",
    "     \\  /PEFIA/      ",
    "      \\/__OS/        ",
    "      /    /         ",
    "     /____/          ",
};

#define LOGO_LINES ((int)(sizeof(LOGO) / sizeof(LOGO[0])))
#define LOGO_COLW  15

/* Print one neofetch row: logo column (left) + "label value" (right). */
static void row(const char *label, const char *value)
{
    int i = a_row++;
    const char *l = (i < LOGO_LINES) ? LOGO[i] : "";

    con_setcolor(a_logo, a_bg);
    con_write(l);
    for (int pad = LOGO_COLW - (int)kstrlen(l); pad > 0; pad--)
        con_putchar(' ');

    con_setcolor(a_key, a_bg);
    con_write(label);
    con_setcolor(a_val, a_bg);
    con_write(value);
    con_putchar('\n');
}

static void color_swatches(void)
{
    int i = a_row++;
    const char *l = (i < LOGO_LINES) ? LOGO[i] : "";
    con_setcolor(a_logo, a_bg);
    con_write(l);
    for (int pad = LOGO_COLW - (int)kstrlen(l); pad > 0; pad--)
        con_putchar(' ');

    color_t palette[8] = {
        fb_rgb(40, 42, 54),  fb_rgb(255, 85, 85),  fb_rgb(80, 250, 123),
        fb_rgb(241, 250, 140), fb_rgb(98, 114, 164), fb_rgb(255, 121, 198),
        fb_rgb(139, 233, 253), fb_rgb(248, 248, 242),
    };
    for (int k = 0; k < 8; k++) {
        con_setcolor(a_val, palette[k]);
        con_write("  ");
    }
    con_setcolor(a_val, a_bg);
    con_putchar('\n');
}

static void cmd_about(void)
{
    a_logo = fb_rgb(120, 200, 255);
    a_key  = fb_rgb(255, 210, 90);
    a_val  = fb_rgb(220, 220, 220);
    a_bg   = fb_rgb(12, 12, 20);
    a_row  = 0;

    /* Dynamic facts. */
    char num[11];
    char mem[24];
    kutoa(g_mem_kb / 1024, num);
    kstrcpy(mem, num); kstrcat(mem, " MB");

    char w[11], h[11], d[11], res[40];
    kutoa((uint32_t)fb_width(),  w);
    kutoa((uint32_t)fb_height(), h);
    kutoa((uint32_t)fb_bpp(),    d);
    kstrcpy(res, w); kstrcat(res, "x"); kstrcat(res, h);
    kstrcat(res, "x"); kstrcat(res, d);

    con_putchar('\n');
    row("pefia", "@pefiaOS");
    row("", "---------------");
    row("OS:      ", "pefiaOS 0.2 (Stardance)");
    row("Host:    ", "x86 PC - BIOS / GRUB2");
    row("Kernel:  ", "32-bit protected mode");
    row("Shell:   ", "pefia-sh");
    row("CPU:     ", "x86 / i686");
    row("Memory:  ", mem);
    row("Display: ", res);
    row("Input:   ", "PS/2 keyboard + mouse");
    color_swatches();
    con_putchar('\n');

    con_setcolor(fb_rgb(200, 200, 200), a_bg);
}

/* --- other built-ins --- */

/* Shell foreground/background colours (set up in shell_run). */
static color_t SH_FG, SH_BG;

/* Print "<label><n> bytes\n" in the current colour. */
static void print_bytes(const char *label, size_t n)
{
    char num[11];
    kutoa((uint32_t)n, num);
    con_write(label); con_write(num); con_write(" bytes\n");
}

/* alloc, stamp+verify a pattern, free every other block, refill, free all. */
static void cmd_memtest(void)
{
    static const int sizes[10] = { 16, 64, 128, 32, 256, 8, 512, 100, 1024, 48 };
    void *blocks[10];
    int   ok = 1;

    size_t before = heap_free_bytes();
    print_bytes("heap free before: ", before);

    for (int i = 0; i < 10; i++) {
        blocks[i] = kmalloc((size_t)sizes[i]);
        if (!blocks[i]) { ok = 0; continue; }
        kmemset(blocks[i], 0xAB, (size_t)sizes[i]);
    }
    for (int i = 0; i < 10 && ok; i++) {
        uint8_t *b = (uint8_t *)blocks[i];
        if (!b) { ok = 0; break; }
        for (int j = 0; j < sizes[i]; j++)
            if (b[j] != 0xAB) { ok = 0; break; }
    }
    for (int i = 0; i < 10; i += 2) kfree(blocks[i]);
    for (int i = 0; i < 10; i += 2) {
        blocks[i] = kmalloc((size_t)sizes[i]);
        if (!blocks[i]) ok = 0;
    }
    for (int i = 0; i < 10; i++) kfree(blocks[i]);

    size_t after = heap_free_bytes();
    print_bytes("heap free after:  ", after);
    if (after != before) ok = 0;   /* leak or coalescing bug */

    con_setcolor(ok ? fb_rgb(120, 230, 120) : fb_rgb(255, 120, 120), SH_BG);
    con_write(ok ? "memtest: PASS\n" : "memtest: FAIL\n");
    con_setcolor(SH_FG, SH_BG);
}

static void cmd_help(void)
{
    con_write("Built-in commands:\n");
    con_write("  help        show this list\n");
    con_write("  about       neofetch-style system info\n");
    con_write("  memtest     stress-test the kernel heap\n");
    con_write("  clear       clear the screen\n");
    con_write("  echo TEXT   print TEXT back\n");
}

/* --- shell loop --- */

static void print_prompt(void)
{
    con_setcolor(fb_rgb(120, 230, 120), SH_BG);
    con_write("pefia");
    con_setcolor(fb_rgb(120, 200, 255), SH_BG);
    con_write("@pefiaOS");
    con_setcolor(SH_FG, SH_BG);
    con_write(":~$ ");
}

static void read_line(char *line)
{
    size_t len = 0;
    for (;;) {
        char c = (char)input_getchar();
        if (c == '\n') { con_putchar('\n'); break; }
        else if (c == '\b') { if (len > 0) { len--; con_putchar('\b'); } }
        else if (len < LINE_MAX - 1) { line[len++] = c; con_putchar(c); }
    }
    line[len] = '\0';
}

void shell_run(void)
{
    char line[LINE_MAX];

    SH_FG = fb_rgb(210, 210, 210);
    SH_BG = fb_rgb(12, 12, 20);
    con_setcolor(SH_FG, SH_BG);
    con_write("Type 'help' for a list of commands. Try 'about'.\n\n");

    for (;;) {
        print_prompt();
        read_line(line);

        const char *echo_arg = str_after_prefix(line, "echo ");

        if (line[0] == '\0') {
            /* empty line */
        } else if (str_eq(line, "help")) {
            cmd_help();
        } else if (str_eq(line, "about")) {
            cmd_about();
        } else if (str_eq(line, "memtest")) {
            cmd_memtest();
        } else if (str_eq(line, "clear")) {
            con_clear();
        } else if (echo_arg) {
            con_write(echo_arg);
            con_putchar('\n');
        } else {
            con_setcolor(fb_rgb(255, 120, 120), SH_BG);
            con_write("Unknown command: ");
            con_write(line);
            con_setcolor(SH_FG, SH_BG);
            con_write("\nType 'help' for a list.\n");
        }
    }
}
