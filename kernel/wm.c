/* kernel/wm.c
 * Window manager + compositor. Windows live in z-order inside wins[], with
 * the last entry always on top. Each frame: paint the desktop gradient,
 * every window back-to-front with a drop shadow, then the taskbar over all
 * of it. Input is polled, never blocking - the taskbar gets first look at
 * clicks (and at keys while its menu is open), otherwise keys go to whatever
 * window is on top and clicks raise/drag/resize/close or fall through to the
 * app underneath.
 */
#include "wm.h"
#include "framebuffer.h"
#include "console.h"
#include "input.h"
#include "mouse.h"
#include "heap.h"
#include "taskbar.h"
#include "explorer.h"
#include "terminal.h"
#include "notepad.h"
#include "browser.h"
#include "games.h"
#include "doom_app.h"

#include <stddef.h>

#define MAX_WINDOWS 16
#define TITLE_H     26
#define CLOSE_SZ    16
#define GLYPH_W     8
#define GLYPH_H     16
#define PAD         10
#define GRIP        16    /* bottom-right resize handle, in pixels */
#define MIN_W       200   /* a window can't be resized smaller than this */
#define MIN_H       150

static Window *wins[MAX_WINDOWS];
static int win_count;

static color_t col_border, col_title, col_title_dim, col_title_text;
static color_t col_content, col_text, col_close, col_shadow;

static void copy_str(char *dst, const char *src, int cap)
{
    int i = 0;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void wm_init(void)
{
    win_count = 0;
    col_border     = fb_rgb(56, 86, 142);
    col_title      = fb_rgb(34, 92, 196);
    col_title_dim  = fb_rgb(84, 98, 128);
    col_title_text = fb_rgb(245, 249, 255);
    col_content    = fb_rgb(246, 249, 255);
    col_text       = fb_rgb(27, 35, 52);
    col_close      = fb_rgb(214, 76, 96);
    col_shadow     = fb_rgb(16, 23, 37);
}

static Window *alloc_window(int x, int y, int w, int h, const char *title)
{
    if (win_count >= MAX_WINDOWS) return NULL;
    Window *win = (Window *)kmalloc(sizeof(Window));
    if (!win) return NULL;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->visible = 1;
    win->cwd = 0; win->sel = -1;
    win->state = NULL;
    win->body[0] = '\0';
    copy_str(win->title, title, (int)sizeof(win->title));
    wins[win_count++] = win;
    return win;
}

Window *wm_create_info(int x, int y, int w, int h, const char *title, const char *body)
{
    Window *win = alloc_window(x, y, w, h, title);
    if (!win) return NULL;
    win->kind = WIN_INFO;
    copy_str(win->body, body, (int)sizeof(win->body));
    return win;
}

Window *wm_create_explorer(int x, int y, int w, int h, const char *title, int dir)
{
    Window *win = alloc_window(x, y, w, h, title);
    if (!win) return NULL;
    win->kind = WIN_EXPLORER;
    win->cwd = dir;
    win->sel = -1;
    return win;
}

Window *wm_create_terminal(int x, int y, int w, int h)
{
    Window *win = alloc_window(x, y, w, h, "Terminal");
    if (!win) return NULL;
    win->kind = WIN_TERMINAL;
    win->state = terminal_new();
    return win;
}

Window *wm_create_notepad(int x, int y, int w, int h)
{
    Window *win = alloc_window(x, y, w, h, "Notepad");
    if (!win) return NULL;
    win->kind = WIN_NOTEPAD;
    win->state = notepad_new();
    return win;
}

Window *wm_create_browser(int x, int y, int w, int h)
{
    Window *win = alloc_window(x, y, w, h, "Browser");
    if (!win) return NULL;
    win->kind = WIN_BROWSER;
    win->state = browser_new();
    return win;
}

Window *wm_create_game(int kind, int x, int y, int w, int h)
{
    Window *win = alloc_window(x, y, w, h, game_title(kind));
    if (!win) return NULL;
    win->kind = WIN_GAME;
    win->state = game_new(kind);
    return win;
}

Window *wm_create_doom(int x, int y, int w, int h)
{
    Window *win = alloc_window(x, y, w, h, "DOOM");
    if (!win) return NULL;
    win->kind = WIN_DOOM;
    win->state = doom_app_new();
    return win;
}

static void draw_text_clipped(int x, int y, const char *s, int max_cols, color_t fg, color_t bg)
{
    char clipped[128];
    if (max_cols <= 0) return;
    if (max_cols > 127) max_cols = 127;
    int i = 0;
    while (s[i] && i < max_cols) { clipped[i] = s[i]; i++; }
    clipped[i] = '\0';
    gfx_text(x, y, clipped, fg, bg);
}

/* Word-wraps win->body (used by WIN_INFO) into the content area, breaking at
 * the last space before a line would overflow rather than mid-word. */
static void draw_info_body(Window *w)
{
    int max_cols = (w->w - 2 * PAD) / GLYPH_W;
    int max_rows = (w->h - TITLE_H - PAD - 4) / GLYPH_H;
    if (max_cols <= 0 || max_rows <= 0) return;
    if (max_cols > 127) max_cols = 127;

    const char *s = w->body;
    int base_y = w->y + TITLE_H + 8;
    char line[128];

    for (int row = 0; row < max_rows && *s; row++) {
        while (*s == ' ') s++;
        if (!*s) break;

        int len = 0, consumed = 0, last_space = -1;
        const char *scan = s;
        while (*scan && len < max_cols) {
            if (*scan == ' ') last_space = len;
            line[len++] = *scan++;
            consumed++;
        }
        if (*scan && *scan != ' ' && last_space > 0) { len = last_space; consumed = last_space; }
        line[len] = '\0';
        gfx_text(w->x + PAD, base_y + row * GLYPH_H, line, col_text, col_content);
        s += consumed;
    }
}

static void draw_window(Window *w, int focused)
{
    fb_fill_rect(w->x + 6, w->y + 6, w->w + 8, w->h + 8, col_shadow);
    fb_fill_rect(w->x + 3, w->y + 3, w->w + 4, w->h + 4, fb_rgb(35, 46, 66));

    color_t title_bg  = focused ? col_title : col_title_dim;
    color_t title_top = focused ? fb_rgb(53, 118, 228) : fb_rgb(102, 114, 144);

    fb_fill_rect(w->x - 1, w->y - 1, w->w + 2, w->h + 2, col_border);
    fb_fill_rect(w->x, w->y, w->w, TITLE_H / 2, title_top);
    fb_fill_rect(w->x, w->y + TITLE_H / 2, w->w, TITLE_H - (TITLE_H / 2), title_bg);

    int title_cols = (w->w - 8 - CLOSE_SZ - 20) / GLYPH_W;
    draw_text_clipped(w->x + 10, w->y + 6, w->title, title_cols, col_title_text, title_bg);

    int close_x = w->x + w->w - CLOSE_SZ - 6, close_y = w->y + 5;
    fb_fill_rect(close_x, close_y, CLOSE_SZ, CLOSE_SZ, col_close);
    fb_fill_rect(close_x, close_y + CLOSE_SZ - 1, CLOSE_SZ, 1, fb_rgb(160, 45, 65));
    gfx_text(close_x + 4, close_y + 1, "x", col_title_text, col_close);

    int body_y = w->y + TITLE_H, body_w = w->w, body_h = w->h - TITLE_H;
    switch (w->kind) {
    case WIN_EXPLORER: explorer_paint(w, w->x, body_y, body_w, body_h); break;
    case WIN_TERMINAL: terminal_paint(w, w->x, body_y, body_w, body_h); break;
    case WIN_NOTEPAD:  notepad_paint(w, w->x, body_y, body_w, body_h); break;
    case WIN_BROWSER:  browser_paint(w, w->x, body_y, body_w, body_h); break;
    case WIN_GAME:     game_paint(w, w->x, body_y, body_w, body_h); break;
    case WIN_DOOM:     doom_app_paint(w, w->x, body_y, body_w, body_h); break;
    default:
        fb_fill_rect(w->x, body_y, body_w, body_h, col_content);
        draw_info_body(w);
        break;
    }

    /* resize grip: three diagonal ticks in the bottom-right corner */
    int grip_x = w->x + w->w, grip_y = w->y + w->h;
    for (int d = 4; d <= 12; d += 4)
        for (int k = 0; k < d; k += 3)
            fb_fill_rect(grip_x - 3 - k, grip_y - 3 - (d - k), 2, 2, col_border);
}

static void draw_desktop(void)
{
    int w = fb_width(), h = fb_height();
    int bands = 64, band_h = (h + bands - 1) / bands;
    for (int i = 0; i < bands; i++) {
        int t = (i * 255) / bands;
        uint8_t r = (uint8_t)(10 + (t * 16) / 255);
        uint8_t g = (uint8_t)(26 + (t * 58) / 255);
        uint8_t b = (uint8_t)(54 + (t * 130) / 255);
        fb_fill_rect(0, i * band_h, w, band_h, fb_rgb(r, g, b));
    }

    fb_fill_rect(0, h - 140, w, 140, fb_rgb(18, 58, 132));
    fb_fill_rect(0, h - 139, w, 1, fb_rgb(74, 126, 218));
}

static void wm_paint(void)
{
    mouse_hide();
    draw_desktop();
    for (int i = 0; i < win_count; i++)
        if (wins[i]->visible)
            draw_window(wins[i], i == win_count - 1);
    taskbar_paint();
    mouse_show();
}

static void raise_to_top(int i)
{
    Window *w = wins[i];
    for (int k = i; k < win_count - 1; k++) wins[k] = wins[k + 1];
    wins[win_count - 1] = w;
}

static void close_index(int i)
{
    switch (wins[i]->kind) {
    case WIN_GAME: game_free(wins[i]->state); break;      /* also frees its offscreen buffer */
    case WIN_DOOM: doom_app_free(wins[i]->state); break;
    default:
        if (wins[i]->state) kfree(wins[i]->state);
        break;
    }
    kfree(wins[i]);
    for (int k = i; k < win_count - 1; k++) wins[k] = wins[k + 1];
    win_count--;
}

static int point_in_window(Window *w, int px, int py)
{
    return px >= w->x - 1 && px < w->x + w->w + 1 &&
           py >= w->y - 1 && py < w->y + w->h + 1;
}

int         wm_count(void)  { return win_count; }
const char *wm_title(int i) { return (i >= 0 && i < win_count) ? wins[i]->title : ""; }
void        wm_raise(int i) { if (i >= 0 && i < win_count) raise_to_top(i); }

void wm_run(void)
{
    int dragging = 0, resizing = 0, drag_dx = 0, drag_dy = 0;
    unsigned prev_buttons = mouse_buttons();
    int prev_mx = mouse_x(), prev_my = mouse_y();

    wm_paint();

    for (;;) {
        for (int i = 0; i < win_count; i++) {
            if (wins[i] && wins[i]->visible && wins[i]->kind == WIN_BROWSER)
                browser_tick(wins[i]);
        }

        /* Only the topmost window gets a tick - games and DOOM pace their own
         * animation, and there's no point advancing a game hidden behind
         * something else. */
        if (win_count > 0 && wins[win_count - 1]->visible) {
            Window *top = wins[win_count - 1];
            if (top->kind == WIN_GAME)
                game_tick(top, top->x, top->y + TITLE_H, top->w, top->h - TITLE_H);
            else if (top->kind == WIN_DOOM)
                doom_app_tick(top, top->x, top->y + TITLE_H, top->w, top->h - TITLE_H);
        }

        int key = input_poll();

        unsigned buttons = mouse_buttons();
        int mx = mouse_x(), my = mouse_y();
        int moved     = (mx != prev_mx || my != prev_my);
        int left_down = (buttons & 1) && !(prev_buttons & 1);
        int left_up   = !(buttons & 1) && (prev_buttons & 1);

        if (key) {
            if (taskbar_menu_open()) {
                taskbar_key((char)key);
                wm_paint();
            } else if (win_count > 0) {
                Window *focused = wins[win_count - 1];
                switch (focused->kind) {
                case WIN_TERMINAL: terminal_key(focused, (char)key); wm_paint(); break;
                case WIN_NOTEPAD:  notepad_key(focused, (char)key);  wm_paint(); break;
                case WIN_BROWSER:  browser_key(focused, (char)key);  wm_paint(); break;
                case WIN_GAME:     game_key(focused, (char)key); break;   /* next tick repaints */
                default: break;
                }
            }
        }

        if (left_down) {
            if (taskbar_click(mx, my)) {
                wm_paint();
            } else {
                for (int i = win_count - 1; i >= 0; i--) {
                    Window *w = wins[i];
                    if (!w->visible || !point_in_window(w, mx, my)) continue;

                    int close_x = w->x + w->w - CLOSE_SZ - 5, close_y = w->y + 5;
                    int on_close = mx >= close_x && mx < close_x + CLOSE_SZ &&
                                   my >= close_y && my < close_y + CLOSE_SZ;
                    int on_title = my >= w->y && my < w->y + TITLE_H;
                    int on_grip  = mx >= w->x + w->w - GRIP && mx < w->x + w->w &&
                                   my >= w->y + w->h - GRIP && my < w->y + w->h;

                    raise_to_top(i);
                    Window *top = wins[win_count - 1];
                    if (on_close) {
                        close_index(win_count - 1);
                    } else if (on_grip) {
                        resizing = 1;
                    } else if (on_title) {
                        dragging = 1;
                        drag_dx = mx - top->x;
                        drag_dy = my - top->y;
                    } else if (top->kind == WIN_EXPLORER) {
                        explorer_click(top, mx - top->x, my - (top->y + TITLE_H));
                    } else if (top->kind == WIN_BROWSER) {
                        browser_click(top, mx - top->x, my - (top->y + TITLE_H));
                    }
                    wm_paint();
                    break;
                }
            }
        }

        if (dragging && (buttons & 1) && moved) {
            Window *w = wins[win_count - 1];
            w->x = mx - drag_dx;
            w->y = my - drag_dy;
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if (w->x > fb_width() - 60)      w->x = fb_width() - 60;
            if (w->y > fb_height() - TITLE_H) w->y = fb_height() - TITLE_H;
            wm_paint();
        }

        if (resizing && (buttons & 1) && moved) {
            Window *w = wins[win_count - 1];
            int new_w = mx - w->x, new_h = my - w->y;
            if (new_w < MIN_W) new_w = MIN_W;
            if (new_h < MIN_H) new_h = MIN_H;
            if (w->x + new_w > fb_width())  new_w = fb_width()  - w->x;
            if (w->y + new_h > fb_height()) new_h = fb_height() - w->y;
            w->w = new_w; w->h = new_h;
            if (w->kind == WIN_GAME)      game_resize(w, w->w, w->h - TITLE_H);
            else if (w->kind == WIN_DOOM) doom_app_resize(w, w->w, w->h - TITLE_H);
            wm_paint();
        }

        if (left_up) { dragging = 0; resizing = 0; }

        prev_buttons = buttons; prev_mx = mx; prev_my = my;
    }
}
