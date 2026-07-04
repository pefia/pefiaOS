/* kernel/shell.c
 * The console-mode shell that used to be the whole UI before the desktop
 * existed. Built-ins: help, about (neofetch knockoff), memtest, free, uptime,
 * history, ver, clear, echo.
 */
#include "shell.h"
#include "console.h"
#include "framebuffer.h"
#include "input.h"
#include "heap.h"
#include "clock.h"
#include "sysinfo.h"
#include "util.h"

#include <stddef.h>

#define LINE_MAX 128

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

/* neofetch-style `about`: a tiny ASCII logo down the left column with
 * "label: value" rows printed alongside it. */
static color_t logo_fg, label_fg, value_fg, about_bg;
static int about_row;

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

static void about_row_print(const char *label, const char *value)
{
    int i = about_row++;
    const char *logo_line = (i < LOGO_LINES) ? LOGO[i] : "";

    con_setcolor(logo_fg, about_bg);
    con_write(logo_line);
    for (int pad = LOGO_COLW - (int)kstrlen(logo_line); pad > 0; pad--)
        con_putchar(' ');

    con_setcolor(label_fg, about_bg);
    con_write(label);
    con_setcolor(value_fg, about_bg);
    con_write(value);
    con_putchar('\n');
}

static void about_color_swatches(void)
{
    int i = about_row++;
    const char *logo_line = (i < LOGO_LINES) ? LOGO[i] : "";
    con_setcolor(logo_fg, about_bg);
    con_write(logo_line);
    for (int pad = LOGO_COLW - (int)kstrlen(logo_line); pad > 0; pad--)
        con_putchar(' ');

    color_t palette[8] = {
        fb_rgb(40, 42, 54),    fb_rgb(255, 85, 85),  fb_rgb(80, 250, 123),
        fb_rgb(241, 250, 140), fb_rgb(98, 114, 164), fb_rgb(255, 121, 198),
        fb_rgb(139, 233, 253), fb_rgb(248, 248, 242),
    };
    for (int k = 0; k < 8; k++) {
        con_setcolor(value_fg, palette[k]);
        con_write("  ");
    }
    con_setcolor(value_fg, about_bg);
    con_putchar('\n');
}

static void cmd_about(void)
{
    logo_fg  = fb_rgb(120, 200, 255);
    label_fg = fb_rgb(255, 210, 90);
    value_fg = fb_rgb(220, 220, 220);
    about_bg = fb_rgb(12, 12, 20);
    about_row = 0;

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
    about_row_print("pefia", "@pefiaOS");
    about_row_print("", "---------------");
    about_row_print("OS:      ", PEFIA_VERSION);
    about_row_print("Host:    ", "x86 PC - BIOS / GRUB2");
    about_row_print("Kernel:  ", "32-bit protected mode");
    about_row_print("Shell:   ", "pefia-sh");
    about_row_print("CPU:     ", "x86 / i686");
    about_row_print("Memory:  ", mem);
    about_row_print("Display: ", res);
    about_row_print("Input:   ", "PS/2 keyboard + mouse");
    about_color_swatches();
    con_putchar('\n');

    con_setcolor(fb_rgb(200, 200, 200), about_bg);
}

/* Foreground/background the rest of the built-ins print with, set once in
 * shell_run and read back here so a command can restore them after using
 * its own colors. */
static color_t shell_fg, shell_bg;

static void print_byte_count(const char *label, size_t n)
{
    char num[11];
    kutoa((uint32_t)n, num);
    con_write(label); con_write(num); con_write(" bytes\n");
}

/* Allocates a spread of block sizes, stamps and re-checks a fill pattern,
 * frees every other block and refills those slots, then frees everything -
 * a quick smoke test that the allocator isn't leaking or corrupting memory. */
static void cmd_memtest(void)
{
    static const int sizes[10] = { 16, 64, 128, 32, 256, 8, 512, 100, 1024, 48 };
    void *blocks[10];
    int   passed = 1;

    size_t before = heap_free_bytes();
    print_byte_count("heap free before: ", before);

    for (int i = 0; i < 10; i++) {
        blocks[i] = kmalloc((size_t)sizes[i]);
        if (!blocks[i]) { passed = 0; continue; }
        kmemset(blocks[i], 0xAB, (size_t)sizes[i]);
    }
    for (int i = 0; i < 10 && passed; i++) {
        uint8_t *b = (uint8_t *)blocks[i];
        if (!b) { passed = 0; break; }
        for (int j = 0; j < sizes[i]; j++)
            if (b[j] != 0xAB) { passed = 0; break; }
    }
    for (int i = 0; i < 10; i += 2) kfree(blocks[i]);
    for (int i = 0; i < 10; i += 2) {
        blocks[i] = kmalloc((size_t)sizes[i]);
        if (!blocks[i]) passed = 0;
    }
    for (int i = 0; i < 10; i++) kfree(blocks[i]);

    size_t after = heap_free_bytes();
    print_byte_count("heap free after:  ", after);
    if (after != before) passed = 0;   /* leak or a coalescing bug */

    con_setcolor(passed ? fb_rgb(120, 230, 120) : fb_rgb(255, 120, 120), shell_bg);
    con_write(passed ? "memtest: PASS\n" : "memtest: FAIL\n");
    con_setcolor(shell_fg, shell_bg);
}

static void cmd_free(void)
{
    size_t total = heap_total_bytes();
    size_t freeb = heap_free_bytes();
    size_t used  = total - freeb;

    print_byte_count("heap total: ", total);
    print_byte_count("heap used:  ", used);
    print_byte_count("heap free:  ", freeb);

    int pct = total ? (int)((uint64_t)used * 100 / total) : 0;
    char num[11];
    con_write("usage: [");
    for (int i = 0; i < 20; i++) con_putchar(i * 5 < pct ? '#' : '-');
    con_write("] ");
    kutoa((uint32_t)pct, num);
    con_write(num);
    con_write("%\n");
}

static void cmd_uptime(void)
{
    uint32_t secs = clock_ms() / 1000;
    char num[11];
    con_write("up ");
    kutoa(secs / 3600, num);      con_write(num); con_write(" h ");
    kutoa((secs / 60) % 60, num); con_write(num); con_write(" m ");
    kutoa(secs % 60, num);        con_write(num); con_write(" s\n");
}

static void cmd_ver(void)
{
    con_write(PEFIA_VERSION " - 32-bit protected mode, i686\n");
}

/* Ring buffer of the last HIST_MAX non-empty commands. hist_n keeps counting
 * up forever so cmd_history can tell how many lines have actually been typed
 * even after the buffer has wrapped. */
#define HIST_MAX 16
static char cmd_history_buf[HIST_MAX][LINE_MAX];
static int  cmd_history_count = 0;

static void history_record(const char *line)
{
    if (!line[0]) return;
    kstrcpy(cmd_history_buf[cmd_history_count % HIST_MAX], line);
    cmd_history_count++;
}

static void cmd_history(void)
{
    int first = cmd_history_count > HIST_MAX ? cmd_history_count - HIST_MAX : 0;
    char num[11];
    for (int i = first; i < cmd_history_count; i++) {
        kutoa((uint32_t)(i + 1), num);
        con_write("  "); con_write(num); con_write("  ");
        con_write(cmd_history_buf[i % HIST_MAX]);
        con_putchar('\n');
    }
}

static void cmd_help(void)
{
    con_write("Built-in commands:\n");
    con_write("  help        show this list\n");
    con_write("  about       neofetch-style system info\n");
    con_write("  memtest     stress-test the kernel heap\n");
    con_write("  free        heap usage (total/used/free + bar)\n");
    con_write("  uptime      time since boot\n");
    con_write("  history     recent commands\n");
    con_write("  ver         OS version\n");
    con_write("  clear       clear the screen\n");
    con_write("  echo TEXT   print TEXT back\n");
}

static void print_prompt(void)
{
    con_setcolor(fb_rgb(120, 230, 120), shell_bg);
    con_write("pefia");
    con_setcolor(fb_rgb(120, 200, 255), shell_bg);
    con_write("@pefiaOS");
    con_setcolor(shell_fg, shell_bg);
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

    shell_fg = fb_rgb(210, 210, 210);
    shell_bg = fb_rgb(12, 12, 20);
    con_setcolor(shell_fg, shell_bg);
    con_write("Type 'help' for a list of commands. Try 'about'.\n\n");

    for (;;) {
        print_prompt();
        read_line(line);

        const char *echo_arg = str_after_prefix(line, "echo ");
        history_record(line);

        if (line[0] == '\0') {
            /* nothing typed, just re-prompt */
        } else if (str_eq(line, "help")) {
            cmd_help();
        } else if (str_eq(line, "about")) {
            cmd_about();
        } else if (str_eq(line, "memtest")) {
            cmd_memtest();
        } else if (str_eq(line, "free")) {
            cmd_free();
        } else if (str_eq(line, "uptime")) {
            cmd_uptime();
        } else if (str_eq(line, "history")) {
            cmd_history();
        } else if (str_eq(line, "ver")) {
            cmd_ver();
        } else if (str_eq(line, "clear")) {
            con_clear();
        } else if (str_eq(line, "echo")) {
            con_putchar('\n');   /* bare echo with no args still prints a blank line */
        } else if (echo_arg) {
            con_write(echo_arg);
            con_putchar('\n');
        } else {
            con_setcolor(fb_rgb(255, 120, 120), shell_bg);
            con_write("Unknown command: ");
            con_write(line);
            con_setcolor(shell_fg, shell_bg);
            con_write("\nType 'help' for a list.\n");
        }
    }
}
