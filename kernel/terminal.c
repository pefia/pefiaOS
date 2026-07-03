/* kernel/terminal.c
 * A real little shell in a window. Maintains its own scrollback + input line
 * and runs commands against the VFS and the heap: help, ls, cat, echo, about,
 * meminfo, clear.
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

#define TROWS 80
#define TCOLS 110

typedef struct {
    char lines[TROWS][TCOLS];
    int  count;
    char input[128];
    int  inlen;
} Term;

static int streq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static const char *afterpfx(const char *s, const char *p)
{
    int i = 0;
    while (p[i]) { if (s[i] != p[i]) return 0; i++; }
    return s + i;
}

static void push(Term *t, const char *s)
{
    if (t->count >= TROWS) {                 /* scroll the buffer up by one */
        for (int r = 0; r < TROWS - 1; r++)
            for (int i = 0; i < TCOLS; i++) t->lines[r][i] = t->lines[r + 1][i];
        t->count = TROWS - 1;
    }
    int i = 0;
    for (; s[i] && i < TCOLS - 1; i++) t->lines[t->count][i] = s[i];
    t->lines[t->count][i] = '\0';
    t->count++;
}

void *terminal_new(void)
{
    Term *t = (Term *)kmalloc(sizeof(Term));
    if (!t) return 0;
    t->count = 0; t->inlen = 0; t->input[0] = '\0';
    push(t, "pefiaOS terminal - type 'help'");
    return t;
}

static void exec(Term *t)
{
    char *cmd = t->input;

    char echo[140];
    int p = 0;
    echo[p++] = '$'; echo[p++] = ' ';
    for (int i = 0; cmd[i] && p < 139; i++) echo[p++] = cmd[i];
    echo[p] = '\0';
    push(t, echo);

    const char *arg;
    if (cmd[0] == '\0') {
        /* nothing */
    } else if (streq(cmd, "help")) {
        push(t, "commands: help  ls  cat <file>  echo <text>  about  meminfo  uptime  clear");
    } else if (streq(cmd, "ls")) {
        int kids[64];
        int n = vfs_children(vfs_root(), kids, 64);
        for (int i = 0; i < n; i++) {
            const VNode *v = vfs_node(kids[i]);
            char line[80];
            int q = 0;
            const char *tag = v->is_dir ? "<dir> " : "      ";
            for (int k = 0; tag[k]; k++) line[q++] = tag[k];
            for (int k = 0; v->name[k] && q < 79; k++) line[q++] = v->name[k];
            line[q] = '\0';
            push(t, line);
        }
    } else if ((arg = afterpfx(cmd, "cat ")) != 0) {
        int kids[64];
        int n = vfs_children(vfs_root(), kids, 64), found = 0;
        for (int i = 0; i < n; i++) {
            const VNode *v = vfs_node(kids[i]);
            if (streq(v->name, arg)) {
                found = 1;
                push(t, v->is_dir ? "(is a directory)" : (v->content ? v->content : "(empty)"));
            }
        }
        if (!found) push(t, "cat: file not found");
    } else if ((arg = afterpfx(cmd, "echo ")) != 0) {
        push(t, arg);
    } else if (streq(cmd, "about")) {
        push(t, PEFIA_VERSION " - 32-bit hobby OS");
    } else if (streq(cmd, "meminfo")) {
        char line[80], num[12];
        int q = 0;
        const char *l = "heap free ";
        for (int i = 0; l[i]; i++) line[q++] = l[i];
        kutoa((uint32_t)heap_free_bytes(), num);
        for (int i = 0; num[i]; i++) line[q++] = num[i];
        const char *l2 = " of ";
        for (int i = 0; l2[i]; i++) line[q++] = l2[i];
        kutoa((uint32_t)heap_total_bytes(), num);
        for (int i = 0; num[i]; i++) line[q++] = num[i];
        const char *l3 = " bytes";
        for (int i = 0; l3[i]; i++) line[q++] = l3[i];
        line[q] = '\0';
        push(t, line);
    } else if (streq(cmd, "uptime")) {
        char line[80], num[12];
        int q = 0;
        uint32_t s = clock_ms() / 1000;
        const char *l = "up ";
        for (int i = 0; l[i]; i++) line[q++] = l[i];
        kutoa(s / 3600, num);
        for (int i = 0; num[i]; i++) line[q++] = num[i];
        line[q++] = 'h'; line[q++] = ' ';
        kutoa((s / 60) % 60, num);
        for (int i = 0; num[i]; i++) line[q++] = num[i];
        line[q++] = 'm'; line[q++] = ' ';
        kutoa(s % 60, num);
        for (int i = 0; num[i]; i++) line[q++] = num[i];
        line[q++] = 's';
        line[q] = '\0';
        push(t, line);
    } else if (streq(cmd, "clear")) {
        t->count = 0;
    } else {
        char line[80];
        int q = 0;
        const char *u = "unknown command: ";
        for (int i = 0; u[i]; i++) line[q++] = u[i];
        for (int i = 0; cmd[i] && q < 79; i++) line[q++] = cmd[i];
        line[q] = '\0';
        push(t, line);
    }
}

void terminal_key(Window *w, char c)
{
    Term *t = (Term *)w->state;
    if (!t) return;
    if (c == '\n')      { exec(t); t->inlen = 0; t->input[0] = '\0'; }
    else if (c == '\b') { if (t->inlen > 0) t->input[--t->inlen] = '\0'; }
    else if (c >= 32 && c < 127 && t->inlen < (int)sizeof(t->input) - 1) {
        t->input[t->inlen++] = c;
        t->input[t->inlen] = '\0';
    }
}

static void tclip(int x, int y, const char *s, int cols, color_t fg, color_t bg)
{
    char b[160];
    if (cols <= 0) return;
    if (cols > 159) cols = 159;
    int i = 0;
    while (s[i] && i < cols) { b[i] = s[i]; i++; }
    b[i] = '\0';
    gfx_text(x, y, b, fg, bg);
}

void terminal_paint(Window *w, int cx, int cy, int cw, int ch)
{
    Term *t = (Term *)w->state;
    color_t bg = fb_rgb(16, 18, 24), fg = fb_rgb(180, 235, 180);

    fb_fill_rect(cx, cy, cw, ch, bg);
    if (!t) return;

    int cols = (cw - 8) / 8;
    int rows = (ch - 6) / 16;
    if (rows < 1) return;

    int show  = rows - 1;                       /* last row is the input line */
    int start = (t->count > show) ? t->count - show : 0;
    int y = cy + 3, r = 0;

    for (int i = start; i < t->count; i++)
        tclip(cx + 4, y + (r++) * 16, t->lines[i], cols, fg, bg);

    char line[160];
    int p = 0;
    line[p++] = '$'; line[p++] = ' ';
    for (int i = 0; t->input[i] && p < 157; i++) line[p++] = t->input[i];
    line[p++] = '_';
    line[p] = '\0';
    tclip(cx + 4, y + r * 16, line, cols, fb_rgb(220, 240, 220), bg);
}
