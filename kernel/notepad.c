/* kernel/notepad.c
 * Type into a buffer; Enter inserts a newline, Backspace deletes. Renders the
 * text wrapped to the window with a blinking-less block cursor at the end.
 * (No saving yet - the VFS is read-only for now.)
 */
#include "notepad.h"
#include "wm.h"
#include "heap.h"
#include "framebuffer.h"
#include "console.h"

#define NP_MAX 4096

typedef struct { char buf[NP_MAX]; int len; } Note;

void *notepad_new(void)
{
    Note *n = (Note *)kmalloc(sizeof(Note));
    if (!n) return 0;
    n->len = 0; n->buf[0] = '\0';
    return n;
}

void notepad_key(Window *w, char c)
{
    Note *n = (Note *)w->state;
    if (!n) return;
    if (c == '\b')      { if (n->len > 0) n->buf[--n->len] = '\0'; }
    else if (c == '\n') { if (n->len < NP_MAX - 1) { n->buf[n->len++] = '\n'; n->buf[n->len] = '\0'; } }
    else if (c >= 32 && c < 127 && n->len < NP_MAX - 1) {
        n->buf[n->len++] = c;
        n->buf[n->len] = '\0';
    }
}

void notepad_paint(Window *w, int cx, int cy, int cw, int ch)
{
    Note *n = (Note *)w->state;
    color_t bg = fb_rgb(250, 250, 245), fg = fb_rgb(30, 30, 35);

    fb_fill_rect(cx, cy, cw, ch, bg);
    if (!n) return;

    int cols = (cw - 12) / 8;
    int rows = (ch - 10) / 16;
    if (cols < 1 || rows < 1) return;

    int x0 = cx + 6, y0 = cy + 5;
    int col = 0, row = 0;

    for (int i = 0; i <= n->len; i++) {
        if (i == n->len) {                              /* draw the cursor */
            if (row < rows) fb_fill_rect(x0 + col * 8, y0 + row * 16, 8, 14, fb_rgb(120, 160, 220));
            break;
        }
        char c = n->buf[i];
        if (c == '\n') { col = 0; row++; if (row >= rows) break; continue; }
        if (col >= cols) { col = 0; row++; if (row >= rows) break; }
        char s[2] = { c, '\0' };
        gfx_text(x0 + col * 8, y0 + row * 16, s, fg, bg);
        col++;
    }
}
