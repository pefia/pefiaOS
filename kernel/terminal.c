/* kernel/terminal.c
 * A little shell that lives in a window instead of on the boot console: its
 * own scrollback buffer and input line, running commands against the VFS and
 * heap (help, ls, cat, echo, about, meminfo, uptime, clear).
 */
#include "terminal.h"
#include "wm.h"
#include "vfs.h"
#include "heap.h"
#include "clock.h"
#include "sysinfo.h"
#include "framebuffer.h"
#include "console.h"
#include "util.h"

#define TERM_ROWS 80
#define TERM_COLS 110

typedef struct {
    char lines[TERM_ROWS][TERM_COLS];
    int  count;
    char input[128];
    int  inlen;
} Term;

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static const char *str_after_prefix(const char *s, const char *prefix)
{
    int i = 0;
    while (prefix[i]) { if (s[i] != prefix[i]) return 0; i++; }
    return s + i;
}

/* Small fixed-buffer line builder so the command handlers below don't each
 * hand-roll their own append loop over a raw char array. */
typedef struct { char buf[TERM_COLS]; int len; } LineBuf;

static void lb_reset(LineBuf *lb) { lb->len = 0; lb->buf[0] = '\0'; }

static void lb_str(LineBuf *lb, const char *s)
{
    while (*s && lb->len < TERM_COLS - 1) lb->buf[lb->len++] = *s++;
    lb->buf[lb->len] = '\0';
}

static void lb_uint(LineBuf *lb, uint32_t v)
{
    char digits[11];
    kutoa(v, digits);
    lb_str(lb, digits);
}

static void scroll_up(Term *t)
{
    for (int r = 0; r < TERM_ROWS - 1; r++)
        kmemmove(t->lines[r], t->lines[r + 1], TERM_COLS);
    t->count = TERM_ROWS - 1;
}

static void push_line(Term *t, const char *s)
{
    if (t->count >= TERM_ROWS) scroll_up(t);
    int i = 0;
    for (; s[i] && i < TERM_COLS - 1; i++) t->lines[t->count][i] = s[i];
    t->lines[t->count][i] = '\0';
    t->count++;
}

void *terminal_new(void)
{
    Term *t = (Term *)kmalloc(sizeof(Term));
    if (!t) return 0;
    t->count = 0;
    t->inlen = 0;
    t->input[0] = '\0';
    push_line(t, "pefiaOS terminal - type 'help'");
    return t;
}

static void run_ls(Term *t)
{
    int kids[64];
    int n = vfs_children(vfs_root(), kids, 64);
    for (int i = 0; i < n; i++) {
        const VNode *v = vfs_node(kids[i]);
        LineBuf lb;
        lb_reset(&lb);
        lb_str(&lb, v->is_dir ? "<dir> " : "      ");
        lb_str(&lb, v->name);
        push_line(t, lb.buf);
    }
}

static void run_cat(Term *t, const char *filename)
{
    int kids[64];
    int n = vfs_children(vfs_root(), kids, 64);
    int found = 0;
    for (int i = 0; i < n; i++) {
        const VNode *v = vfs_node(kids[i]);
        if (str_eq(v->name, filename)) {
            found = 1;
            push_line(t, v->is_dir ? "(is a directory)" : (v->content ? v->content : "(empty)"));
        }
    }
    if (!found) push_line(t, "cat: file not found");
}

static void run_meminfo(Term *t)
{
    LineBuf lb;
    lb_reset(&lb);
    lb_str(&lb, "heap free ");
    lb_uint(&lb, (uint32_t)heap_free_bytes());
    lb_str(&lb, " of ");
    lb_uint(&lb, (uint32_t)heap_total_bytes());
    lb_str(&lb, " bytes");
    push_line(t, lb.buf);
}

static void run_uptime(Term *t)
{
    uint32_t secs = clock_ms() / 1000;
    LineBuf lb;
    lb_reset(&lb);
    lb_str(&lb, "up ");
    lb_uint(&lb, secs / 3600); lb_str(&lb, "h ");
    lb_uint(&lb, (secs / 60) % 60); lb_str(&lb, "m ");
    lb_uint(&lb, secs % 60); lb_str(&lb, "s");
    push_line(t, lb.buf);
}

static void execute(Term *t)
{
    const char *cmd = t->input;

    LineBuf echoed;
    lb_reset(&echoed);
    lb_str(&echoed, "$ ");
    lb_str(&echoed, cmd);
    push_line(t, echoed.buf);

    const char *arg;
    if (cmd[0] == '\0') {
        /* blank line, nothing to do */
    } else if (str_eq(cmd, "help")) {
        push_line(t, "commands: help  ls  cat <file>  echo <text>  about  meminfo  uptime  clear");
    } else if (str_eq(cmd, "ls")) {
        run_ls(t);
    } else if ((arg = str_after_prefix(cmd, "cat ")) != 0) {
        run_cat(t, arg);
    } else if ((arg = str_after_prefix(cmd, "echo ")) != 0) {
        push_line(t, arg);
    } else if (str_eq(cmd, "about")) {
        push_line(t, PEFIA_VERSION " - 32-bit hobby OS");
    } else if (str_eq(cmd, "meminfo")) {
        run_meminfo(t);
    } else if (str_eq(cmd, "uptime")) {
        run_uptime(t);
    } else if (str_eq(cmd, "clear")) {
        t->count = 0;
    } else {
        LineBuf lb;
        lb_reset(&lb);
        lb_str(&lb, "unknown command: ");
        lb_str(&lb, cmd);
        push_line(t, lb.buf);
    }
}

void terminal_key(Window *w, char c)
{
    Term *t = (Term *)w->state;
    if (!t) return;

    if (c == '\n') {
        execute(t);
        t->inlen = 0;
        t->input[0] = '\0';
    } else if (c == '\b') {
        if (t->inlen > 0) t->input[--t->inlen] = '\0';
    } else if (c >= 32 && c < 127 && t->inlen < (int)sizeof(t->input) - 1) {
        t->input[t->inlen++] = c;
        t->input[t->inlen] = '\0';
    }
}

static void draw_clipped(int x, int y, const char *s, int max_cols, color_t fg, color_t bg)
{
    char clipped[160];
    if (max_cols <= 0) return;
    if (max_cols > 159) max_cols = 159;
    int i = 0;
    while (s[i] && i < max_cols) { clipped[i] = s[i]; i++; }
    clipped[i] = '\0';
    gfx_text(x, y, clipped, fg, bg);
}

void terminal_paint(Window *w, int cx, int cy, int cw, int ch)
{
    Term *t = (Term *)w->state;
    color_t bg = fb_rgb(16, 18, 24);
    color_t fg = fb_rgb(180, 235, 180);

    fb_fill_rect(cx, cy, cw, ch, bg);
    if (!t) return;

    int cols = (cw - 8) / 8;
    int rows = (ch - 6) / 16;
    if (rows < 1) return;

    int visible_rows = rows - 1;   /* bottom row is reserved for the input line */
    int first = (t->count > visible_rows) ? t->count - visible_rows : 0;
    int y = cy + 3, row = 0;

    for (int i = first; i < t->count; i++)
        draw_clipped(cx + 4, y + (row++) * 16, t->lines[i], cols, fg, bg);

    LineBuf prompt;
    lb_reset(&prompt);
    lb_str(&prompt, "$ ");
    lb_str(&prompt, t->input);
    lb_str(&prompt, "_");
    draw_clipped(cx + 4, y + row * 16, prompt.buf, cols, fb_rgb(220, 240, 220), bg);
}
