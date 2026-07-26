#include "terminal.h"
#include "wm.h"
#include "input.h"
#include "vfs.h"
#include "heap.h"
#include "clock.h"
#include "sched.h"
#include "idt.h"
#include "sound.h"
#include "ata.h"
#include "sysinfo.h"
#include "framebuffer.h"
#include "console.h"
#include "util.h"
#include "interp.h"

#define TERM_ROWS 80
#define TERM_COLS 110
#define HIST_MAX  16

typedef struct {
    char lines[TERM_ROWS][TERM_COLS];
    int  count;
    char input[128];
    int  inlen;
    int  cwd;                       /* current directory, a VFS node index */
    char hist[HIST_MAX][128];
    int  hist_count;                /* total ever entered; index modulo HIST_MAX */
    int  hist_pos;                  /* Up/Down recall cursor; == hist_count when not browsing */
} Term;

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* Small fixed-buffer line builder so the command handlers don't each hand-roll
 * their own append loop over a raw char array. */
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
    t->cwd = vfs_root();
    t->hist_count = 0;
    t->hist_pos = 0;
    push_line(t, "pefiaOS terminal - type 'help'");
    return t;
}

/* Absolute-ish path label for the prompt/pwd: walk parents up to root. Only a
 * few levels deep in practice, so a small fixed buffer is plenty. */
static void path_of(int dir, char *out, int cap)
{
    char stack[8][32];
    int depth = 0;
    int cur = dir;
    while (cur > 0 && depth < 8) {
        const VNode *n = vfs_node(cur);
        if (!n) break;
        int i = 0; while (n->name[i] && i < 31) { stack[depth][i] = n->name[i]; i++; } stack[depth][i] = '\0';
        depth++;
        cur = n->parent;
    }
    int p = 0;
    if (depth == 0) { if (cap > 1) out[p++] = '/'; out[p] = '\0'; return; }
    for (int d = depth - 1; d >= 0; d--) {
        if (p < cap - 1) out[p++] = '/';
        for (int i = 0; stack[d][i] && p < cap - 1; i++) out[p++] = stack[d][i];
    }
    out[p] = '\0';
}

static void run_ls(Term *t)
{
    int kids[64];
    int n = vfs_children(t->cwd, kids, 64);
    if (n == 0) { push_line(t, "(empty)"); return; }
    for (int i = 0; i < n; i++) {
        const VNode *v = vfs_node(kids[i]);
        if (!v) continue;
        LineBuf lb; lb_reset(&lb);
        lb_str(&lb, v->is_dir ? "<dir> " : "      ");
        lb_str(&lb, v->name);
        if (!v->is_dir) { lb_str(&lb, "  "); lb_uint(&lb, (uint32_t)v->size); lb_str(&lb, "b"); }
        push_line(t, lb.buf);
    }
}

static void run_cat(Term *t, const char *filename)
{
    int idx = vfs_find_child(t->cwd, filename);
    const VNode *v = vfs_node(idx);
    if (!v)            { push_line(t, "cat: file not found"); return; }
    if (v->is_dir)     { push_line(t, "cat: is a directory"); return; }
    if (!v->content)   { push_line(t, "(empty)"); return; }
    /* Print the body a line at a time so newlines in the file wrap correctly. */
    char line[TERM_COLS]; int p = 0;
    for (const char *s = v->content; ; s++) {
        if (*s == '\n' || *s == '\0') {
            line[p] = '\0'; push_line(t, line); p = 0;
            if (*s == '\0') break;
        } else if (p < TERM_COLS - 1) {
            line[p++] = *s;
        }
    }
}

static void run_cd(Term *t, const char *name)
{
    if (!name[0] || str_eq(name, "/")) { t->cwd = vfs_root(); return; }
    if (str_eq(name, "..")) {
        const VNode *d = vfs_node(t->cwd);
        if (d && d->parent >= 0) t->cwd = d->parent;
        return;
    }
    int idx = vfs_find_child(t->cwd, name);
    const VNode *v = vfs_node(idx);
    if (!v)          push_line(t, "cd: no such directory");
    else if (!v->is_dir) push_line(t, "cd: not a directory");
    else             t->cwd = idx;
}

static void run_mkdir(Term *t, const char *name)
{
    if (vfs_create(t->cwd, name, 1) < 0) push_line(t, "mkdir: failed (name taken or table full)");
}

static void run_touch(Term *t, const char *name)
{
    if (vfs_find_child(t->cwd, name) >= 0) return;
    if (vfs_create(t->cwd, name, 0) < 0) push_line(t, "touch: failed");
}

static void run_rm(Term *t, const char *name)
{
    int idx = vfs_find_child(t->cwd, name);
    if (idx < 0)                 push_line(t, "rm: no such file");
    else if (vfs_delete(idx) < 0) push_line(t, "rm: failed (directory not empty?)");
}

/* Create-or-overwrite `name` in cwd with `text`. Backs `echo ... > file`. */
static void write_file(Term *t, const char *name, const char *text)
{
    int idx = vfs_find_child(t->cwd, name);
    if (idx < 0) idx = vfs_create(t->cwd, name, 0);
    if (idx < 0) { push_line(t, "write: could not create file"); return; }
    int len = 0; while (text[len]) len++;
    if (vfs_write(idx, text, len) < 0) push_line(t, "write: failed");
}

static void run_meminfo(Term *t)
{
    LineBuf lb; lb_reset(&lb);
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
    LineBuf lb; lb_reset(&lb);
    lb_str(&lb, "up ");
    lb_uint(&lb, secs / 3600); lb_str(&lb, "h ");
    lb_uint(&lb, (secs / 60) % 60); lb_str(&lb, "m ");
    lb_uint(&lb, secs % 60); lb_str(&lb, "s   ticks=");
    lb_uint(&lb, timer_ticks());
    push_line(t, lb.buf);
}

/* disk reads sector 0 off the primary ATA drive and shows a hex + ASCII dump
 * of the first 16 bytes - proof the PIO driver is actually talking to hardware. */
static void run_disk(Term *t)
{
    if (!ata_present()) { push_line(t, "disk: no ATA drive on the primary channel"); return; }
    static uint8_t sec[512];
    if (ata_read(0, 1, sec) != 0) { push_line(t, "disk: read error"); return; }

    LineBuf lb; lb_reset(&lb);
    lb_str(&lb, "sector0: ");
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        char h[4] = { hexd[sec[i] >> 4], hexd[sec[i] & 0xF], ' ', 0 };
        lb_str(&lb, h);
    }
    push_line(t, lb.buf);

    lb_reset(&lb);
    lb_str(&lb, "ascii:   ");
    for (int i = 0; i < 16; i++) {
        char c[2] = { (sec[i] >= 32 && sec[i] < 127) ? (char)sec[i] : '.', 0 };
        lb_str(&lb, c);
    }
    push_line(t, lb.buf);
}

/* ps lists the kernel scheduler's threads (see sched.c). */
static void run_ps(Term *t)
{
    push_line(t, "  TID  STATE     NAME");
    int n = sched_count();
    for (int i = 0; i < n; i++) {
        LineBuf lb; lb_reset(&lb);
        lb_str(&lb, "  ");
        lb_uint(&lb, (uint32_t)sched_tid(i));
        lb_str(&lb, "    ");
        lb_str(&lb, sched_state(i));
        lb_str(&lb, "  ");
        lb_str(&lb, sched_name(i));
        push_line(t, lb.buf);
    }
}

static void run_history(Term *t)
{
    int first = t->hist_count > HIST_MAX ? t->hist_count - HIST_MAX : 0;
    for (int i = first; i < t->hist_count; i++) {
        LineBuf lb; lb_reset(&lb);
        lb_str(&lb, "  ");
        lb_uint(&lb, (uint32_t)(i + 1));
        lb_str(&lb, "  ");
        lb_str(&lb, t->hist[i % HIST_MAX]);
        push_line(t, lb.buf);
    }
}

/* Split `line` into a command word (up to the first space) and the remaining
 * argument string (leading spaces trimmed). Returns the arg pointer. */
static const char *split_arg(const char *line, char *cmd, int cap)
{
    int i = 0;
    while (line[i] && line[i] != ' ' && i < cap - 1) { cmd[i] = line[i]; i++; }
    cmd[i] = '\0';
    while (line[i] == ' ') i++;
    return line + i;
}

static void execute(Term *t)
{
    char raw[128];
    { int i = 0; for (; t->input[i] && i < 127; i++) raw[i] = t->input[i]; raw[i] = '\0'; }

    LineBuf echoed; lb_reset(&echoed);
    lb_str(&echoed, "$ "); lb_str(&echoed, raw);
    push_line(t, echoed.buf);

    if (raw[0] == '\0') return;

    /* Redirection: split off a "> file" tail before dispatch. Everything left
     * of '>' is the command; the (single) word after it is the target file. */
    char redir[32]; redir[0] = '\0';
    for (int i = 0; raw[i]; i++) {
        if (raw[i] == '>') {
            raw[i] = '\0';
            int j = i + 1; while (raw[j] == ' ' || raw[j] == '>') j++;
            int k = 0; while (raw[j] && raw[j] != ' ' && k < 31) redir[k++] = raw[j++];
            redir[k] = '\0';
            /* trim trailing spaces the command word picked up */
            int e = i; while (e > 0 && raw[e - 1] == ' ') raw[--e] = '\0';
            break;
        }
    }

    char cmd[32];
    const char *arg = split_arg(raw, cmd, sizeof(cmd));

    /* `echo ... > file` is the one redirect we special-case into a real write;
     * for other commands `>` just names a file we drop a one-line note into. */
    if (redir[0] && str_eq(cmd, "echo")) { write_file(t, redir, arg); return; }

    if (str_eq(cmd, "help")) {
        push_line(t, "files:  ls  cd <d>  pwd  cat <f>  mkdir <d>  touch <f>  rm <f>");
        push_line(t, "        echo <text> > <file>   (write a file)");
        push_line(t, "system: ps  meminfo  uptime  history  about  clear  beep  disk");
        push_line(t, "python: py <file.py>   (runs it in a Python window)");
    } else if (str_eq(cmd, "ls")) {
        run_ls(t);
    } else if (str_eq(cmd, "pwd")) {
        char path[128]; path_of(t->cwd, path, sizeof(path)); push_line(t, path);
    } else if (str_eq(cmd, "cd")) {
        run_cd(t, arg);
    } else if (str_eq(cmd, "cat")) {
        run_cat(t, arg);
    } else if (str_eq(cmd, "mkdir")) {
        run_mkdir(t, arg);
    } else if (str_eq(cmd, "touch")) {
        run_touch(t, arg);
    } else if (str_eq(cmd, "rm")) {
        run_rm(t, arg);
    } else if (str_eq(cmd, "echo")) {
        push_line(t, arg);
    } else if (str_eq(cmd, "ps")) {
        run_ps(t);
    } else if (str_eq(cmd, "about")) {
        push_line(t, PEFIA_VERSION " - 32-bit hobby OS");
    } else if (str_eq(cmd, "meminfo")) {
        run_meminfo(t);
    } else if (str_eq(cmd, "uptime")) {
        run_uptime(t);
    } else if (str_eq(cmd, "history")) {
        run_history(t);
    } else if (str_eq(cmd, "clear")) {
        t->count = 0;
    } else if (str_eq(cmd, "beep")) {
        sound_beep(880, 150);
    } else if (str_eq(cmd, "disk")) {
        run_disk(t);
    } else if (str_eq(cmd, "py")) {
        int idx = vfs_find_child(t->cwd, arg);
        const VNode *v = (idx >= 0) ? vfs_node(idx) : 0;
        if (!v || v->is_dir) {
            push_line(t, "py: no such file");
        } else {
            Window *win = wm_create_interp(INTERP_PY, 180, 80, 640, 460);
            if (win && win->state) interp_run_source(win->state, v->content ? v->content : "");
        }
    } else {
        LineBuf lb; lb_reset(&lb);
        lb_str(&lb, "unknown command: "); lb_str(&lb, cmd);
        push_line(t, lb.buf);
    }

    if (redir[0]) {   /* non-echo redirect: leave a breadcrumb so it's not silent */
        LineBuf note; lb_reset(&note);
        lb_str(&note, "(only 'echo' can redirect into "); lb_str(&note, redir); lb_str(&note, ")");
        push_line(t, note.buf);
    }
}

void terminal_key(Window *w, char c)
{
    Term *t = (Term *)w->state;
    if (!t) return;

    /* Up/Down walk the command history into the input line. Compare as
     * unsigned: the KEY_* codes are 0x81+ and char is signed here. */
    unsigned char uc = (unsigned char)c;
    if (uc == KEY_UP || uc == KEY_DOWN) {
        int first = t->hist_count > HIST_MAX ? t->hist_count - HIST_MAX : 0;
        if (uc == KEY_UP && t->hist_pos > first)            t->hist_pos--;
        else if (uc == KEY_DOWN && t->hist_pos < t->hist_count) t->hist_pos++;
        else return;
        t->inlen = 0;
        if (t->hist_pos < t->hist_count) {   /* stepping past the newest clears the line */
            const char *h = t->hist[t->hist_pos % HIST_MAX];
            while (h[t->inlen] && t->inlen < (int)sizeof(t->input) - 1) { t->input[t->inlen] = h[t->inlen]; t->inlen++; }
        }
        t->input[t->inlen] = '\0';
        return;
    }

    if (c == '\n') {
        if (t->input[0]) {
            int i = 0; for (; t->input[i] && i < 127; i++) t->hist[t->hist_count % HIST_MAX][i] = t->input[i];
            t->hist[t->hist_count % HIST_MAX][i] = '\0';
            t->hist_count++;
        }
        t->hist_pos = t->hist_count;
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

    char path[128]; path_of(t->cwd, path, sizeof(path));
    LineBuf prompt; lb_reset(&prompt);
    lb_str(&prompt, path); lb_str(&prompt, " $ "); lb_str(&prompt, t->input); lb_str(&prompt, "_");
    draw_clipped(cx + 4, y + row * 16, prompt.buf, cols, fb_rgb(220, 240, 220), bg);
}
