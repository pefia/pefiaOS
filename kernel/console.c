#include "console.h"
#include "framebuffer.h"
#include "font8x16.h"
#include "mouse.h"
#include "util.h"

static int origin_x, origin_y;   /* pixel origin of the console region */
static int col_count, row_count;
static int cursor_x, cursor_y;
static color_t ink, paper;

static void draw_glyph(int px, int py, unsigned char ch, color_t f, color_t b)
{
    const unsigned char *cov = font8x16aa[ch & 0x7F];
    for (int row = 0; row < FONT_HEIGHT; row++)
        for (int col = 0; col < FONT_WIDTH; col++)
            fb_put_pixel(px + col, py + row,
                         fb_mix(b, f, cov[row * FONT_WIDTH + col]));
}

void gfx_text(int x, int y, const char *s, color_t f, color_t b)
{
    for (int i = 0; s[i]; i++)
        draw_glyph(x + i * FONT_WIDTH, y, (unsigned char)s[i], f, b);
}

void con_init(int x, int y, int c, int r, color_t f, color_t b)
{
    origin_x = x; origin_y = y; col_count = c; row_count = r;
    ink = f; paper = b; cursor_x = 0; cursor_y = 0;
    fb_fill_rect(origin_x, origin_y, col_count * FONT_WIDTH, row_count * FONT_HEIGHT, paper);
}

void con_setcolor(color_t f, color_t b) { ink = f; paper = b; }

void con_clear(void)
{
    mouse_hide();
    fb_fill_rect(origin_x, origin_y, col_count * FONT_WIDTH, row_count * FONT_HEIGHT, paper);
    cursor_x = 0; cursor_y = 0;
    mouse_show();
}

static void wrap_line(void)
{
    cursor_x = 0;
    if (++cursor_y >= row_count) {
        fb_scroll_up(origin_x, origin_y, col_count * FONT_WIDTH, row_count * FONT_HEIGHT,
                     FONT_HEIGHT, paper);
        cursor_y = row_count - 1;
    }
}

/* Draws one character without touching the mouse cursor - callers that
 * push out a whole string want to hide/show the cursor exactly once,
 * not per character. */
static void emit_char(char ch)
{
    switch (ch) {
    case '\n': wrap_line(); break;
    case '\r': cursor_x = 0; break;
    case '\t': for (int i = 0; i < 4; i++) emit_char(' '); break;
    case '\b':
        if (cursor_x > 0) cursor_x--; else if (cursor_y > 0) { cursor_y--; cursor_x = col_count - 1; }
        draw_glyph(origin_x + cursor_x * FONT_WIDTH, origin_y + cursor_y * FONT_HEIGHT, ' ', ink, paper);
        break;
    default:
        draw_glyph(origin_x + cursor_x * FONT_WIDTH, origin_y + cursor_y * FONT_HEIGHT,
                   (unsigned char)ch, ink, paper);
        if (++cursor_x >= col_count) wrap_line();
        break;
    }
}

/* The public entry points hide the mouse cursor before drawing and restore
 * it after, so its saved-under pixels never end up with our text baked
 * into them. */
void con_putchar(char ch)
{
    mouse_hide();
    emit_char(ch);
    mouse_show();
}

void con_write(const char *s)
{
    mouse_hide();
    for (int i = 0; s[i]; i++) emit_char(s[i]);
    mouse_show();
}
