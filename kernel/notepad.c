#include "notepad.h"
#include "wm.h"
#include "heap.h"
#include "framebuffer.h"
#include "console.h"
#include "vfs.h"
#include "explorer.h"

#define NOTE_CAPACITY 8192
#define GUTTER_W      44
#define STATUS_H      18      /* status/name bar along the bottom */

#define KEY_CTRL_O 15
#define KEY_CTRL_R 18
#define KEY_CTRL_S 19
#define KEY_ESC    27

typedef struct {
    char buf[NOTE_CAPACITY];
    int  len;
    char name[32];     /* "" until the file has been named */
    int  dir;          /* vfs directory the file lives in / will be saved to */
    int  node;         /* vfs node index, -1 until first saved */
    int  naming;       /* 0 = editing text, 1 = renaming, 2 = naming before save */
    char namebuf[32];
    int  namelen;
    int  truncated;    /* file didn't fit in buf at open; saving would lose the tail */
    char status[48];   /* one-line feedback shown in the bottom bar */
} NoteDoc;

static void set_status(NoteDoc *doc, const char *msg)
{
    int i = 0;
    while (msg[i] && i < (int)sizeof(doc->status) - 1) { doc->status[i] = msg[i]; i++; }
    doc->status[i] = '\0';
}

void *notepad_new(void)
{
    NoteDoc *doc = (NoteDoc *)kmalloc(sizeof(NoteDoc));
    if (!doc) return 0;
    doc->len = 0;
    doc->buf[0] = '\0';
    doc->name[0] = '\0';
    doc->dir  = vfs_root();
    doc->node = -1;
    doc->naming = 0;
    doc->namelen = 0; doc->namebuf[0] = '\0';
    doc->truncated = 0;
    set_status(doc, "new file - Ctrl+S to save");
    return doc;
}

void *notepad_open(int dir, int node)
{
    NoteDoc *doc = (NoteDoc *)notepad_new();
    if (!doc) return 0;
    doc->dir  = dir;
    doc->node = node;
    const VNode *v = vfs_node(node);
    if (v) {
        int i = 0;
        while (v->name[i] && i < 31) { doc->name[i] = v->name[i]; i++; }
        doc->name[i] = '\0';
        if (v->content) {
            int n = 0;
            while (v->content[n] && n < NOTE_CAPACITY - 1) { doc->buf[n] = v->content[n]; n++; }
            doc->buf[n] = '\0';
            doc->len = n;
            /* If we stopped on the cap rather than the terminator, the tail
             * didn't make it in - saving this buffer would destroy it. */
            doc->truncated = (v->content[n] != '\0');
        }
        set_status(doc, doc->truncated ? "file too large - opened read-only" : "");
    }
    return doc;
}

static void update_title(Window *w, NoteDoc *doc)
{
    const char *n = doc->name[0] ? doc->name : "Notepad";
    int i = 0;
    while (n[i] && i < (int)sizeof(w->title) - 1) { w->title[i] = n[i]; i++; }
    w->title[i] = '\0';
}

static void do_save(Window *w, NoteDoc *doc)
{
    if (doc->truncated) {         /* buf holds only part of the file - a save
                                   * would write the short copy back over it */
        set_status(doc, "file too large - opened read-only");
        return;
    }
    if (!doc->name[0]) {          /* no name yet: ask for one, then save */
        doc->naming = 2;
        doc->namelen = 0; doc->namebuf[0] = '\0';
        return;
    }
    if (doc->node < 0)
        doc->node = vfs_find_child(doc->dir, doc->name);
    if (doc->node < 0)
        doc->node = vfs_create(doc->dir, doc->name, 0);
    if (doc->node < 0 || vfs_write(doc->node, doc->buf, doc->len) != 0) {
        set_status(doc, "save failed");
        return;
    }
    update_title(w, doc);
    set_status(doc, "saved");
}

static void naming_key(Window *w, NoteDoc *doc, char c)
{
    if (c == '\n') {
        doc->namebuf[doc->namelen] = '\0';
        if (doc->namelen == 0) { doc->naming = 0; set_status(doc, "cancelled"); return; }
        if (doc->node >= 0) {
            if (vfs_rename(doc->node, doc->namebuf) != 0) { set_status(doc, "name taken"); return; }
        } else if (vfs_find_child(doc->dir, doc->namebuf) >= 0 && doc->naming == 1) {
            set_status(doc, "name taken");
            return;
        }
        int i = 0;
        while (doc->namebuf[i]) { doc->name[i] = doc->namebuf[i]; i++; }
        doc->name[i] = '\0';
        int save_after = (doc->naming == 2);
        doc->naming = 0;
        update_title(w, doc);
        set_status(doc, "renamed");
        if (save_after) do_save(w, doc);
    } else if (c == KEY_ESC) {
        doc->naming = 0;
        set_status(doc, "cancelled");
    } else if (c == '\b') {
        if (doc->namelen > 0) doc->namebuf[--doc->namelen] = '\0';
    } else if (c > 32 && c < 127 && c != '/' && doc->namelen < 31) {
        doc->namebuf[doc->namelen++] = c;
        doc->namebuf[doc->namelen] = '\0';
    }
}

void notepad_key(Window *w, char c)
{
    NoteDoc *doc = (NoteDoc *)w->state;
    if (!doc) return;

    if (doc->naming) { naming_key(w, doc, c); return; }

    if (c == KEY_CTRL_S) { do_save(w, doc); return; }
    if (c == KEY_CTRL_R) {
        doc->naming = 1;
        doc->namelen = 0;
        while (doc->name[doc->namelen]) { doc->namebuf[doc->namelen] = doc->name[doc->namelen]; doc->namelen++; }
        doc->namebuf[doc->namelen] = '\0';
        return;
    }
    if (c == KEY_CTRL_O) { app_launch_explorer(); return; }

    if (c == '\b') {
        if (doc->len > 0) doc->buf[--doc->len] = '\0';
        return;
    }
    if (c == '\n') {
        if (doc->len < NOTE_CAPACITY - 1) { doc->buf[doc->len++] = '\n'; doc->buf[doc->len] = '\0'; }
        return;
    }
    if (c >= 32 && c < 127 && doc->len < NOTE_CAPACITY - 1) {
        doc->buf[doc->len++] = c;
        doc->buf[doc->len] = '\0';
    }
}

static const char *KEYWORDS[] = {
    "int", "char", "void", "long", "short", "unsigned", "signed", "float",
    "double", "struct", "enum", "union", "const", "static", "return", "if",
    "else", "for", "while", "do", "switch", "case", "break", "continue",
    "default", "sizeof", "typedef", "goto", "extern", "volatile",
    /* Python words so .py files light up too */
    "def", "import", "print", "True", "False", "None", "elif", "in",
    "not", "and", "or", "pass", "range", "len", 0
};

static int is_word(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; }
static int is_alpha_(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }

static int word_is_keyword(const char *s, int len)
{
    for (int k = 0; KEYWORDS[k]; k++) {
        int i = 0;
        while (i < len && KEYWORDS[k][i] && s[i] == KEYWORDS[k][i]) i++;
        if (i == len && KEYWORDS[k][i] == '\0') return 1;
    }
    return 0;
}

/* Fill colr[i] with the highlight color for line[i]. One left-to-right pass:
 * comments and strings swallow the rest of their run, words get checked against
 * the keyword table, digit-led runs are numbers. */
static void highlight_line(const char *line, int n, color_t *colr,
                           color_t base, color_t kw, color_t num, color_t str, color_t cmt)
{
    int i = 0;
    while (i < n) {
        if ((line[i] == '/' && i + 1 < n && line[i + 1] == '/') || line[i] == '#') {
            while (i < n) colr[i++] = cmt;
        } else if (line[i] == '"' || line[i] == '\'') {
            char q = line[i]; colr[i++] = str;
            while (i < n) { colr[i] = str; if (line[i] == q) { i++; break; } i++; }
        } else if (line[i] >= '0' && line[i] <= '9') {
            while (i < n && (is_word(line[i]))) colr[i++] = num;
        } else if (is_alpha_(line[i])) {
            int start = i;
            while (i < n && is_word(line[i])) i++;
            color_t col = word_is_keyword(line + start, i - start) ? kw : base;
            for (int k = start; k < i; k++) colr[k] = col;
        } else {
            colr[i++] = base;
        }
    }
}

void notepad_paint(Window *w, int cx, int cy, int cw, int ch)
{
    NoteDoc *doc = (NoteDoc *)w->state;
    color_t bg     = fb_rgb(30, 32, 40);
    color_t gutter = fb_rgb(22, 24, 30);
    color_t lineno = fb_rgb(110, 120, 140);
    color_t base   = fb_rgb(225, 228, 235);
    color_t kw     = fb_rgb(120, 180, 250);
    color_t num    = fb_rgb(240, 190, 120);
    color_t strc   = fb_rgb(150, 220, 150);
    color_t cmt    = fb_rgb(110, 130, 120);
    color_t bar    = fb_rgb(40, 46, 62);
    color_t bar_fg = fb_rgb(190, 200, 220);

    int text_h = ch - STATUS_H;
    fb_fill_rect(cx, cy, cw, text_h, bg);
    fb_fill_rect(cx, cy, GUTTER_W, text_h, gutter);
    fb_fill_rect(cx, cy + text_h, cw, STATUS_H, bar);
    if (!doc) return;

    /* status bar: name prompt while naming, otherwise name + shortcuts */
    {
        char line[96]; int p = 0;
        const char *parts[6]; int np = 0;
        if (doc->naming) {
            parts[np++] = (doc->naming == 2) ? "save as: " : "rename: ";
            parts[np++] = doc->namebuf;
            parts[np++] = "_";
        } else {
            parts[np++] = doc->name[0] ? doc->name : "(unnamed)";
            parts[np++] = "   ^S save  ^R rename  ^O open   ";
            parts[np++] = doc->status;
        }
        for (int k = 0; k < np; k++)
            for (int i = 0; parts[k][i] && p < 95; i++) line[p++] = parts[k][i];
        line[p] = '\0';
        int cols = (cw - 12) / 8;
        if (cols > 0) {
            if (p > cols) line[cols] = '\0';
            gfx_text(cx + 6, cy + text_h + 1, line, bar_fg, bar);
        }
    }

    int text_x = cx + GUTTER_W + 4;
    int cols = (cw - GUTTER_W - 8) / 8;
    int rows = (text_h - 8) / 16;
    if (cols < 1 || rows < 1) return;

    int y0 = cy + 4;
    int row = 0, lineno_n = 1;
    int i = 0;

    while (row < rows) {
        /* slice out one logical line [i, eol) */
        int start = i;
        while (i < doc->len && doc->buf[i] != '\n') i++;
        int n = i - start;
        if (n > cols) n = cols;

        char ln[8]; int p = 0, v = lineno_n;
        char tmp[8]; int tn = 0;
        if (v == 0) tmp[tn++] = '0';
        while (v) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
        while (tn) ln[p++] = tmp[--tn];
        ln[p] = '\0';
        gfx_text(cx + GUTTER_W - 8 - p * 8, y0 + row * 16, ln, lineno, gutter);

        color_t colr[128];
        if (n > 128) n = 128;
        highlight_line(doc->buf + start, n, colr, base, kw, num, strc, cmt);
        for (int k = 0; k < n; k++) {
            char g[2] = { doc->buf[start + k], '\0' };
            gfx_text(text_x + k * 8, y0 + row * 16, g, colr[k], bg);
        }

        /* draw the cursor block when we're on the last line */
        if (i >= doc->len) {
            fb_fill_rect(text_x + n * 8, y0 + row * 16, 8, 14, fb_rgb(120, 160, 220));
            break;
        }
        i++;
        row++; lineno_n++;
    }
}
