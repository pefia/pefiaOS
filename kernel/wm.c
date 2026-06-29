/* kernel/wm.c
 * -----------------------------------------------------------------------------
 * Window manager + compositor. Windows live in z-order in wins[] (last = top).
 * Each repaint: gradient desktop, every window back-to-front (with a drop
 * shadow), then the taskbar on top. Input is polled non-blocking: the taskbar
 * gets first crack at clicks and at keys while its menu is open; otherwise
 * keys go to the focused (top) window's app, and clicks raise / drag / close /
 * or pass into the app body.
 * -----------------------------------------------------------------------------
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

#define MAXW     16
#define TITLEH   26
#define CLOSE_SZ 16
#define GLYPH_W  8
#define GLYPH_H  16
#define PAD      10
#define GRIP     16      /* bottom-right resize handle */
#define MIN_W    200     /* smallest a window may be dragged to */
#define MIN_H    150

static Window *wins[MAXW];
static int nwins;

static color_t c_border, c_title, c_titledim, c_titletext, c_content, c_text, c_close, c_shadow;

static void copy_str(char *dst, const char *src, int cap)
{
    int i = 0;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void wm_init(void)
{
    nwins = 0;
    c_border    = fb_rgb(56, 86, 142);
    c_title     = fb_rgb(34, 92, 196);
    c_titledim  = fb_rgb(84, 98, 128);
    c_titletext = fb_rgb(245, 249, 255);
    c_content   = fb_rgb(246, 249, 255);
    c_text      = fb_rgb(27, 35, 52);
    c_close     = fb_rgb(214, 76, 96);
    c_shadow    = fb_rgb(16, 23, 37);
}

static Window *alloc_window(int x, int y, int w, int h, const char *title)
{
    if (nwins >= MAXW) return NULL;
    Window *win = (Window *)kmalloc(sizeof(Window));
    if (!win) return NULL;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->visible = 1;
    win->cwd = 0; win->sel = -1;
    win->state = NULL;
    win->body[0] = '\0';
    copy_str(win->title, title, (int)sizeof(win->title));
    wins[nwins++] = win;
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
    win->cwd = dir; win->sel = -1;
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

static void draw_clipped(int x, int y, const char *s, int maxcols, color_t fg, color_t bg)
{
    char buf[128];
    if (maxcols <= 0) return;
    if (maxcols > 127) maxcols = 127;
    int i = 0;
    while (s[i] && i < maxcols) { buf[i] = s[i]; i++; }
    buf[i] = '\0';
    gfx_text(x, y, buf, fg, bg);
}

static void draw_body(Window *w)
{
    int maxcols = (w->w - 2 * PAD) / GLYPH_W;
    int maxrows = (w->h - TITLEH - PAD - 4) / GLYPH_H;
    if (maxcols <= 0 || maxrows <= 0) return;
    if (maxcols > 127) maxcols = 127;

    const char *s = w->body;
    int baseY = w->y + TITLEH + 8;
    char line[128];

    for (int row = 0; row < maxrows && *s; row++) {
        while (*s == ' ') s++;
        if (!*s) break;
        int len = 0, taken = 0, lastSpace = -1;
        const char *scan = s;
        while (*scan && len < maxcols) {
            if (*scan == ' ') lastSpace = len;
            line[len++] = *scan++;
            taken++;
        }
        if (*scan && *scan != ' ' && lastSpace > 0) { len = lastSpace; taken = lastSpace; }
        line[len] = '\0';
        gfx_text(w->x + PAD, baseY + row * GLYPH_H, line, c_text, c_content);
        s += taken;
    }
}

static void draw_window(Window *w, int focused)
{
    fb_fill_rect(w->x + 6, w->y + 6, w->w + 8, w->h + 8, c_shadow);
    fb_fill_rect(w->x + 3, w->y + 3, w->w + 4, w->h + 4, fb_rgb(35, 46, 66));

    color_t tb = focused ? c_title : c_titledim;
    color_t tb2 = focused ? fb_rgb(53, 118, 228) : fb_rgb(102, 114, 144);

    fb_fill_rect(w->x - 1, w->y - 1, w->w + 2, w->h + 2, c_border);
    fb_fill_rect(w->x, w->y, w->w, TITLEH / 2, tb2);
    fb_fill_rect(w->x, w->y + TITLEH / 2, w->w, TITLEH - (TITLEH / 2), tb);

    int title_cols = (w->w - 8 - CLOSE_SZ - 20) / GLYPH_W;
    draw_clipped(w->x + 10, w->y + 6, w->title, title_cols, c_titletext, tb);

    int bx = w->x + w->w - CLOSE_SZ - 6, by = w->y + 5;
    fb_fill_rect(bx, by, CLOSE_SZ, CLOSE_SZ, c_close);
    fb_fill_rect(bx, by + CLOSE_SZ - 1, CLOSE_SZ, 1, fb_rgb(160, 45, 65));
    gfx_text(bx + 4, by + 1, "x", c_titletext, c_close);

    int cy = w->y + TITLEH, cw = w->w, chh = w->h - TITLEH;
    if      (w->kind == WIN_EXPLORER) explorer_paint(w, w->x, cy, cw, chh);
    else if (w->kind == WIN_TERMINAL) terminal_paint(w, w->x, cy, cw, chh);
    else if (w->kind == WIN_NOTEPAD)  notepad_paint(w, w->x, cy, cw, chh);
    else if (w->kind == WIN_BROWSER)  browser_paint(w, w->x, cy, cw, chh);
    else if (w->kind == WIN_GAME)     game_paint(w, w->x, cy, cw, chh);
    else if (w->kind == WIN_DOOM)     doom_app_paint(w, w->x, cy, cw, chh);
    else {
        fb_fill_rect(w->x, cy, cw, chh, c_content);
        draw_body(w);
    }

    /* resize grip: three diagonal ticks in the bottom-right corner */
    int gxp = w->x + w->w, gyp = w->y + w->h;
    for (int d = 4; d <= 12; d += 4)
        for (int k = 0; k < d; k += 3)
            fb_fill_rect(gxp - 3 - k, gyp - 3 - (d - k), 2, 2, c_border);
}

static void draw_desktop(void)
{
    int W = fb_width(), H = fb_height();
    int bands = 64, bh = (H + bands - 1) / bands;
    for (int i = 0; i < bands; i++) {
        int t = (i * 255) / bands;
        uint8_t r = (uint8_t)(10 + (t * 16) / 255);
        uint8_t g = (uint8_t)(26 + (t * 58) / 255);
        uint8_t b = (uint8_t)(54 + (t * 130) / 255);
        fb_fill_rect(0, i * bh, W, bh, fb_rgb(r, g, b));
    }

    fb_fill_rect(0, H - 140, W, 140, fb_rgb(18, 58, 132));
    fb_fill_rect(0, H - 139, W, 1, fb_rgb(74, 126, 218));
}

static void wm_paint(void)
{
    mouse_hide();
    draw_desktop();
    for (int i = 0; i < nwins; i++)
        if (wins[i]->visible)
            draw_window(wins[i], i == nwins - 1);
    taskbar_paint();
    mouse_show();
}

static void raise_to_top(int i)
{
    Window *w = wins[i];
    for (int k = i; k < nwins - 1; k++) wins[k] = wins[k + 1];
    wins[nwins - 1] = w;
}

static void close_index(int i)
{
    if (wins[i]->kind == WIN_GAME)      game_free(wins[i]->state);   /* frees its buffer too */
    else if (wins[i]->kind == WIN_DOOM) doom_app_free(wins[i]->state);
    else if (wins[i]->state)            kfree(wins[i]->state);
    kfree(wins[i]);
    for (int k = i; k < nwins - 1; k++) wins[k] = wins[k + 1];
    nwins--;
}

static int inside(Window *w, int px, int py)
{
    return px >= w->x - 1 && px < w->x + w->w + 1 &&
           py >= w->y - 1 && py < w->y + w->h + 1;
}

int         wm_count(void)  { return nwins; }
const char *wm_title(int i) { return (i >= 0 && i < nwins) ? wins[i]->title : ""; }
void        wm_raise(int i) { if (i >= 0 && i < nwins) raise_to_top(i); }

void wm_run(void)
{
    int dragging = 0, resizing = 0, drag_dx = 0, drag_dy = 0;
    unsigned prev_btns = mouse_buttons();
    int prev_mx = mouse_x(), prev_my = mouse_y();

    wm_paint();

    for (;;) {
        for (int i = 0; i < nwins; i++) {
            if (wins[i] && wins[i]->visible && wins[i]->kind == WIN_BROWSER) {
                browser_tick(wins[i]);
            }
        }

        /* Animate the focused game/DOOM every iteration (they self-pace). */
        if (nwins > 0 && wins[nwins - 1]->visible) {
            Window *t = wins[nwins - 1];
            if (t->kind == WIN_GAME)
                game_tick(t, t->x, t->y + TITLEH, t->w, t->h - TITLEH);
            else if (t->kind == WIN_DOOM)
                doom_app_tick(t, t->x, t->y + TITLEH, t->w, t->h - TITLEH);
        }

        int key = input_poll();

        unsigned b = mouse_buttons();
        int mx = mouse_x(), my = mouse_y();
        int moved     = (mx != prev_mx || my != prev_my);
        int left_down = (b & 1) && !(prev_btns & 1);
        int left_up   = !(b & 1) && (prev_btns & 1);

        if (key) {
            if (taskbar_menu_open()) { taskbar_key((char)key); wm_paint(); }
            else if (nwins > 0) {
                Window *f = wins[nwins - 1];
                if (f->kind == WIN_TERMINAL) { terminal_key(f, (char)key); wm_paint(); }
                else if (f->kind == WIN_NOTEPAD) { notepad_key(f, (char)key); wm_paint(); }
                else if (f->kind == WIN_BROWSER) { browser_key(f, (char)key); wm_paint(); }
                else if (f->kind == WIN_GAME) { game_key(f, (char)key); /* next tick repaints */ }
            }
        }

        if (left_down) {
            if (taskbar_click(mx, my)) {
                wm_paint();
            } else {
                for (int i = nwins - 1; i >= 0; i--) {
                    Window *w = wins[i];
                    if (!w->visible || !inside(w, mx, my)) continue;

                    int bx = w->x + w->w - CLOSE_SZ - 5, by = w->y + 5;
                    int on_close = mx >= bx && mx < bx + CLOSE_SZ &&
                                   my >= by && my < by + CLOSE_SZ;
                    int on_title = my >= w->y && my < w->y + TITLEH;
                    int on_grip  = mx >= w->x + w->w - GRIP && mx < w->x + w->w &&
                                   my >= w->y + w->h - GRIP && my < w->y + w->h;

                    raise_to_top(i);
                    Window *tw = wins[nwins - 1];
                    if (on_close) {
                        close_index(nwins - 1);
                    } else if (on_grip) {
                        resizing = 1;
                    } else if (on_title) {
                        dragging = 1;
                        drag_dx = mx - tw->x;
                        drag_dy = my - tw->y;
                    } else if (tw->kind == WIN_EXPLORER) {
                        explorer_click(tw, mx - tw->x, my - (tw->y + TITLEH));
                    } else if (tw->kind == WIN_BROWSER) {
                        browser_click(tw, mx - tw->x, my - (tw->y + TITLEH));
                    }
                    wm_paint();
                    break;
                }
            }
        }

        if (dragging && (b & 1) && moved) {
            Window *w = wins[nwins - 1];
            w->x = mx - drag_dx;
            w->y = my - drag_dy;
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if (w->x > fb_width() - 60)      w->x = fb_width() - 60;
            if (w->y > fb_height() - TITLEH) w->y = fb_height() - TITLEH;
            wm_paint();
        }

        if (resizing && (b & 1) && moved) {
            Window *w = wins[nwins - 1];
            int nw = mx - w->x, nh = my - w->y;
            if (nw < MIN_W) nw = MIN_W;
            if (nh < MIN_H) nh = MIN_H;
            if (w->x + nw > fb_width())  nw = fb_width()  - w->x;
            if (w->y + nh > fb_height()) nh = fb_height() - w->y;
            w->w = nw; w->h = nh;
            if (w->kind == WIN_GAME)      game_resize(w, w->w, w->h - TITLEH);
            else if (w->kind == WIN_DOOM) doom_app_resize(w, w->w, w->h - TITLEH);
            wm_paint();
        }

        if (left_up) { dragging = 0; resizing = 0; }

        prev_btns = b; prev_mx = mx; prev_my = my;
    }
}
