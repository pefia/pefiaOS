/* kernel/notepad.c
 * A plain text buffer with a cursor at the end - Enter for newline, Backspace
 * to delete. Nothing gets saved anywhere yet since the VFS is read-only.
 */
#include "notepad.h"
#include "wm.h"
#include "heap.h"
#include "framebuffer.h"
#include "console.h"

#define NOTE_CAPACITY 4096

typedef struct {
    char buf[NOTE_CAPACITY];
    int  len;
} NoteDoc;

void *notepad_new(void)
{
    NoteDoc *doc = (NoteDoc *)kmalloc(sizeof(NoteDoc));
    if (!doc) return 0;
    doc->len = 0;
    doc->buf[0] = '\0';
    return doc;
}

void notepad_key(Window *w, char c)
{
    NoteDoc *doc = (NoteDoc *)w->state;
    if (!doc) return;

    if (c == '\b') {
        if (doc->len > 0) doc->buf[--doc->len] = '\0';
        return;
    }
    if (c == '\n') {
        if (doc->len < NOTE_CAPACITY - 1) {
            doc->buf[doc->len++] = '\n';
            doc->buf[doc->len] = '\0';
        }
        return;
    }
    if (c >= 32 && c < 127 && doc->len < NOTE_CAPACITY - 1) {
        doc->buf[doc->len++] = c;
        doc->buf[doc->len] = '\0';
    }
}

void notepad_paint(Window *w, int cx, int cy, int cw, int ch)
{
    NoteDoc *doc = (NoteDoc *)w->state;
    color_t bg = fb_rgb(250, 250, 245);
    color_t fg = fb_rgb(30, 30, 35);

    fb_fill_rect(cx, cy, cw, ch, bg);
    if (!doc) return;

    int cols = (cw - 12) / 8;
    int rows = (ch - 10) / 16;
    if (cols < 1 || rows < 1) return;

    int x0 = cx + 6, y0 = cy + 5;
    int col = 0, row = 0;

    for (int i = 0; i <= doc->len; i++) {
        if (i == doc->len) {
            if (row < rows)
                fb_fill_rect(x0 + col * 8, y0 + row * 16, 8, 14, fb_rgb(120, 160, 220));
            break;
        }

        char c = doc->buf[i];
        if (c == '\n') {
            col = 0; row++;
            if (row >= rows) break;
            continue;
        }
        if (col >= cols) {
            col = 0; row++;
            if (row >= rows) break;
        }

        char glyph[2] = { c, '\0' };
        gfx_text(x0 + col * 8, y0 + row * 16, glyph, fg, bg);
        col++;
    }
}
