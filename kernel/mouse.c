#include "mouse.h"
#include "framebuffer.h"

#define CURSOR_W 12
#define CURSOR_H 19

/* 'X' outlines the arrow, '.' is the white fill, ' ' is left transparent. */
static const char *cursor_glyph[CURSOR_H] = {
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

static int cur_x, cur_y;
static int wheel_accum;
static unsigned button_mask;
static int cursor_enabled = 0, cursor_visible = 0;
static int save_x, save_y, save_w, save_h;
static color_t saved_pixels[CURSOR_H * CURSOR_W];
static color_t color_outline, color_fill;

void mouse_hide(void)
{
    if (!cursor_enabled || !cursor_visible) return;
    for (int row = 0; row < save_h; row++)
        for (int col = 0; col < save_w; col++)
            fb_put_pixel(save_x + col, save_y + row, saved_pixels[row * CURSOR_W + col]);
    cursor_visible = 0;
}

void mouse_show(void)
{
    if (!cursor_enabled || cursor_visible) return;

    save_x = cur_x; save_y = cur_y; save_w = CURSOR_W; save_h = CURSOR_H;
    if (save_x + save_w > fb_width())  save_w = fb_width()  - save_x;
    if (save_y + save_h > fb_height()) save_h = fb_height() - save_y;
    if (save_w < 0) save_w = 0;
    if (save_h < 0) save_h = 0;

    for (int row = 0; row < save_h; row++)
        for (int col = 0; col < save_w; col++)
            saved_pixels[row * CURSOR_W + col] = fb_get_pixel(save_x + col, save_y + row);

    for (int row = 0; row < save_h; row++) {
        for (int col = 0; col < save_w; col++) {
            char mark = cursor_glyph[row][col];
            if (mark == 'X') fb_put_pixel(cur_x + col, cur_y + row, color_outline);
            else if (mark == '.') fb_put_pixel(cur_x + col, cur_y + row, color_fill);
        }
    }
    cursor_visible = 1;
}

void mouse_move(int dx, int dy)
{
    mouse_hide();
    cur_x += dx; cur_y += dy;
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x > fb_width() - 1)  cur_x = fb_width() - 1;
    if (cur_y > fb_height() - 1) cur_y = fb_height() - 1;
    mouse_show();
}

void mouse_set_buttons(unsigned b)
{
    button_mask = b;
    /* Give some visible feedback while the left button is held down. */
    color_fill = (button_mask & 0x1) ? fb_rgb(120, 230, 120) : fb_rgb(255, 255, 255);
}

void mouse_cursor_init(int x, int y)
{
    color_outline = fb_rgb(0, 0, 0);
    color_fill    = fb_rgb(255, 255, 255);
    cur_x = x; cur_y = y;
    cursor_enabled = 1; cursor_visible = 0;
    mouse_show();
}

int mouse_x(void) { return cur_x; }
int mouse_y(void) { return cur_y; }
unsigned mouse_buttons(void) { return button_mask; }

void mouse_add_wheel(int notches) { wheel_accum += notches; }
int  mouse_take_wheel(void) { int n = wheel_accum; wheel_accum = 0; return n; }
