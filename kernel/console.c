/* kernel/console.c */
#include "console.h"
#include "framebuffer.h"
#include "font8x16.h"
#include "mouse.h"
#include "util.h"

static int ox, oy;           /* pixel origin of the console region */
static int cols, rows;       /* size in character cells */
static int cx, cy;           /* cursor position in cells */
static color_t fg, bg;

static void draw_glyph(int px, int py, unsigned char ch, color_t f, color_t b)
{
    const unsigned char *g = font8x16[ch & 0x7F];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        unsigned char bits = g[row];
        for (int col = 0; col < FONT_WIDTH; col++)
            fb_put_pixel(px + col, py + row,
                         (bits & (0x80 >> col)) ? f : b);
    }
}

void gfx_text(int x, int y, const char *s, color_t f, color_t b)
{
    for (int i = 0; s[i]; i++)
        draw_glyph(x + i * FONT_WIDTH, y, (unsigned char)s[i], f, b);
}

void con_init(int x, int y, int c, int r, color_t f, color_t b)
{
    ox = x; oy = y; cols = c; rows = r; fg = f; bg = b; cx = 0; cy = 0;
    fb_fill_rect(ox, oy, cols * FONT_WIDTH, rows * FONT_HEIGHT, bg);
}

void con_setcolor(color_t f, color_t b) { fg = f; bg = b; }

void con_clear(void)
{
    mouse_hide();
    fb_fill_rect(ox, oy, cols * FONT_WIDTH, rows * FONT_HEIGHT, bg);
    cx = 0; cy = 0;
    mouse_show();
}

static void newline(void)
{
    cx = 0;
    if (++cy >= rows) {
        fb_scroll_up(ox, oy, cols * FONT_WIDTH, rows * FONT_HEIGHT,
                     FONT_HEIGHT, bg);
        cy = rows - 1;
    }
}

/* Internal: draw one char without touching the mouse cursor. */
static void putc_raw(char ch)
{
    switch (ch) {
    case '\n': newline(); break;
    case '\r': cx = 0; break;
    case '\t': for (int i = 0; i < 4; i++) putc_raw(' '); break;
    case '\b':
        if (cx > 0) cx--; else if (cy > 0) { cy--; cx = cols - 1; }
        draw_glyph(ox + cx * FONT_WIDTH, oy + cy * FONT_HEIGHT, ' ', fg, bg);
        break;
    default:
        draw_glyph(ox + cx * FONT_WIDTH, oy + cy * FONT_HEIGHT,
                   (unsigned char)ch, fg, bg);
        if (++cx >= cols) newline();
        break;
    }
}

/* Public draw ops hide the mouse cursor first so its saved-under pixels never
 * capture text, then redraw it on top afterwards. */
void con_putchar(char ch)
{
    mouse_hide();
    putc_raw(ch);
    mouse_show();
}

void con_write(const char *s)
{
    mouse_hide();
    for (int i = 0; s[i]; i++) putc_raw(s[i]);
    mouse_show();
}
