/* kernel/mouse.c */
#include "mouse.h"
#include "framebuffer.h"

#define CW 12     /* cursor bitmap width  */
#define CH 19     /* cursor bitmap height */

/* 'X' = black outline, '.' = white fill, ' ' = transparent. */
static const char *cursor[CH] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X.....XXXXXX",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      XXXX  ",
};

static int mx, my;
static unsigned btns;
static int enabled = 0, shown = 0;
static int sx, sy, sw, sh;            /* saved region */
static color_t under[CH * CW];        /* pixels behind the cursor */
static color_t col_outline, col_fill;

void mouse_hide(void)
{
    if (!enabled || !shown) return;
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++)
            fb_put_pixel(sx + i, sy + j, under[j * CW + i]);
    shown = 0;
}

void mouse_show(void)
{
    if (!enabled || shown) return;

    sx = mx; sy = my; sw = CW; sh = CH;
    if (sx + sw > fb_width())  sw = fb_width()  - sx;
    if (sy + sh > fb_height()) sh = fb_height() - sy;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;

    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++)
            under[j * CW + i] = fb_get_pixel(sx + i, sy + j);

    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) {
            char m = cursor[j][i];
            if (m == 'X') fb_put_pixel(mx + i, my + j, col_outline);
            else if (m == '.') fb_put_pixel(mx + i, my + j, col_fill);
        }
    }
    shown = 1;
}

void mouse_move(int dx, int dy)
{
    mouse_hide();
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > fb_width() - 1)  mx = fb_width() - 1;
    if (my > fb_height() - 1) my = fb_height() - 1;
    mouse_show();
}

void mouse_set_buttons(unsigned b)
{
    btns = b;
    /* Tint the cursor while the left button is held, for visible feedback. */
    col_fill = (btns & 0x1) ? fb_rgb(120, 230, 120) : fb_rgb(255, 255, 255);
}

void mouse_cursor_init(int x, int y)
{
    col_outline = fb_rgb(0, 0, 0);
    col_fill    = fb_rgb(255, 255, 255);
    mx = x; my = y;
    enabled = 1; shown = 0;
    mouse_show();
}

int mouse_x(void) { return mx; }
int mouse_y(void) { return my; }
unsigned mouse_buttons(void) { return btns; }

