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
#include "interp.h"
#include "settings.h"
#include "theme.h"
#include "sched.h"
#include "vfs.h"

#include <stddef.h>

#define MAX_WINDOWS 16
#define TITLE_H     26
#define CLOSE_SZ    16
#define GLYPH_W     8
#define GLYPH_H     16
#define PAD         10
#define GRIP        16
#define MIN_W       200   /* a window can't be resized smaller than this */
#define MIN_H       150
#define TASKBAR_H   140   /* reserved strip at the bottom (matches draw_desktop) */
#define SNAP_EDGE   6     /* cursor within this many px of a screen edge snaps */

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
    win->snapped = 0;
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

Window *wm_create_editor(int x, int y, int w, int h, int dir, int node)
{
    const VNode *v = vfs_node(node);
    Window *win = alloc_window(x, y, w, h, v ? v->name : "Notepad");
    if (!win) return NULL;
    win->kind = WIN_NOTEPAD;
    win->state = notepad_open(dir, node);
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

Window *wm_create_interp(int lang, int x, int y, int w, int h)
{
    Window *win = alloc_window(x, y, w, h, lang == INTERP_PY ? "Python" : "C Interpreter");
    if (!win) return NULL;
    win->kind = WIN_INTERP;
    win->state = interp_new(lang);
    return win;
}

Window *wm_create_settings(int x, int y, int w, int h)
{
    Window *win = alloc_window(x, y, w, h, "Settings");
    if (!win) return NULL;
    win->kind = WIN_SETTINGS;
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

    uint8_t ar, ag, ab; theme_accent_rgb(&ar, &ag, &ab);
    color_t accent    = fb_rgb(ar, ag, ab);
    color_t accent_lt = fb_rgb((uint8_t)(ar + (255 - ar) / 4),
                               (uint8_t)(ag + (255 - ag) / 4),
                               (uint8_t)(ab + (255 - ab) / 4));
    color_t title_bg  = focused ? accent    : col_title_dim;
    color_t title_top = focused ? accent_lt : fb_rgb(102, 114, 144);
    (void)col_title;

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
    case WIN_INTERP:   interp_paint(w, w->x, body_y, body_w, body_h); break;
    case WIN_SETTINGS: settings_paint(w, w->x, body_y, body_w, body_h); break;
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
    uint8_t ar, ag, ab; theme_accent_rgb(&ar, &ag, &ab);
    int dark = theme_dark();

    /* Fade a near-black (dark) or near-white (light) top into a dim tint of
     * the accent at the bottom, so the whole desktop follows the theme. */
    int top_r = dark ? 8  : 232, top_g = dark ? 12 : 236, top_b = dark ? 22 : 244;
    int bot_r = dark ? ar / 3 : 160 + ar / 4;
    int bot_g = dark ? ag / 3 : 160 + ag / 4;
    int bot_b = dark ? ab / 3 : 160 + ab / 4;

    int bands = 64, band_h = (h + bands - 1) / bands;
    for (int i = 0; i < bands; i++) {
        int t = (i * 255) / bands;
        uint8_t r = (uint8_t)(top_r + (bot_r - top_r) * t / 255);
        uint8_t g = (uint8_t)(top_g + (bot_g - top_g) * t / 255);
        uint8_t b = (uint8_t)(top_b + (bot_b - top_b) * t / 255);
        fb_fill_rect(0, i * band_h, w, band_h, fb_rgb(r, g, b));
    }

    fb_fill_rect(0, h - 140, w, 140, fb_rgb((uint8_t)(ar / 3 + 6), (uint8_t)(ag / 3 + 8), (uint8_t)(ab / 3 + 12)));
    fb_fill_rect(0, h - 139, w, 1, fb_rgb(ar, ag, ab));
}

static void wm_paint(void)
{
    fb_begin_offscreen();
    mouse_hide();
    draw_desktop();
    for (int i = 0; i < win_count; i++)
        if (wins[i]->visible)
            draw_window(wins[i], i == win_count - 1);
    taskbar_paint();
    mouse_show();
    fb_end_offscreen();
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
    case WIN_GAME: game_free(wins[i]->state); break;
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

/* Set a window's rect and tell size-sensitive apps (game/doom own a buffer). */
static void set_geometry(Window *w, int x, int y, int ww, int hh)
{
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    if (w->kind == WIN_GAME)      game_resize(w, w->w, w->h - TITLE_H);
    else if (w->kind == WIN_DOOM) doom_app_resize(w, w->w, w->h - TITLE_H);
}

/* Edge-tile the top window based on where the drag was released. Saves the
 * pre-snap rect the first time so a later drag can restore it. */
static void snap_to_edge(Window *w, int mx, int my)
{
    int fw = fb_width(), work_h = fb_height() - TASKBAR_H;
    int snap;
    if (my <= SNAP_EDGE)            snap = 1;
    else if (mx <= SNAP_EDGE)       snap = 2;
    else if (mx >= fw - SNAP_EDGE)  snap = 3;
    else return;

    if (!w->snapped) { w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h; }
    w->snapped = 1;
    if (snap == 1)      set_geometry(w, 0,      0, fw,        work_h);
    else if (snap == 2) set_geometry(w, 0,      0, fw / 2,    work_h);
    else                set_geometry(w, fw / 2, 0, fw - fw/2, work_h);
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
        sched_yield();   /* let cooperative kernel threads (e.g. worker) run a slice */

        /* A page whose script timer fired (or whose animated GIF advanced a
         * frame) has nothing to generate an input event, so the pump reports
         * when it changed something and we repaint on its behalf. */
        int pumped = 0;
        for (int i = 0; i < win_count; i++) {
            if (wins[i] && wins[i]->visible && wins[i]->kind == WIN_BROWSER)
                pumped |= browser_tick(wins[i]);
        }
        if (pumped) wm_paint();

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

        /* Wheel goes to the topmost window under the cursor - the same
         * hit-test clicks use - but without raising it. Only the browser
         * has anything to scroll today. */
        int wheel = mouse_take_wheel();
        if (wheel) {
            for (int i = win_count - 1; i >= 0; i--) {
                Window *w = wins[i];
                if (!w->visible || !point_in_window(w, mx, my)) continue;
                if (w->kind == WIN_BROWSER) { browser_scroll(w, wheel); wm_paint(); }
                break;
            }
        }

        if (key) {
            if (taskbar_menu_open()) {
                taskbar_key((char)key);
                wm_paint();
            } else if (win_count > 0) {
                Window *focused = wins[win_count - 1];
                switch (focused->kind) {
                case WIN_TERMINAL: terminal_key(focused, (char)key); wm_paint(); break;
                case WIN_NOTEPAD:  notepad_key(focused, (char)key);  wm_paint(); break;
                case WIN_INTERP:   interp_key(focused, (char)key);   wm_paint(); break;
                case WIN_BROWSER:  browser_key(focused, (char)key);  wm_paint(); break;
                case WIN_GAME:     game_key(focused, (char)key); break;
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
                        if (top->snapped) {          /* pop back to pre-snap size under the cursor */
                            set_geometry(top, mx - top->sw / 2, my - TITLE_H / 2, top->sw, top->sh);
                            if (top->x < 0) top->x = 0;
                            if (top->y < 0) top->y = 0;
                            top->snapped = 0;
                        }
                        dragging = 1;
                        drag_dx = mx - top->x;
                        drag_dy = my - top->y;
                    } else if (top->kind == WIN_EXPLORER) {
                        explorer_click(top, mx - top->x, my - (top->y + TITLE_H));
                    } else if (top->kind == WIN_BROWSER) {
                        browser_click(top, mx - top->x, my - (top->y + TITLE_H));
                    } else if (top->kind == WIN_SETTINGS) {
                        settings_click(top, mx - top->x, my - (top->y + TITLE_H));
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
            w->snapped = 0;
            set_geometry(w, w->x, w->y, new_w, new_h);
            wm_paint();
        }

        if (left_up) {
            if (dragging && win_count > 0) { snap_to_edge(wins[win_count - 1], mx, my); wm_paint(); }
            dragging = 0; resizing = 0;
        }

        prev_buttons = buttons; prev_mx = mx; prev_my = my;
    }
}
