#include "taskbar.h"
#include "wm.h"
#include "explorer.h"
#include "vfs.h"
#include "rtc.h"
#include "framebuffer.h"
#include "console.h"
#include "net.h"
#include "games.h"
#include "interp.h"
#include "theme.h"

#define BAR_H    40
#define START_W  92
#define NET_W    120
#define MENU_W   300
#define MENU_H   452
#define BTN_W    156
#define BTN_GAP  4

#define MENU_ROW_H   24
#define MENU_LIST_TOP 76          /* list top, offset from the menu's top edge */
#define MENU_ARROW_W  16          /* scroll arrow column (shown only when needed) */

static int  menu_open = 0;
static char search_text[40];
static int  search_len = 0;
static int  menu_scroll = 0;

/* List geometry shared by draw + click so they never disagree. Returns the
 * number of rows that fit; fills the list band and per-scroll arrow rects. */
static int menu_geom(int my, int *list_top, int *list_bottom, int *arrow_x,
                     int *up_y, int *down_y)
{
    int lt = my + MENU_LIST_TOP;
    int lb = my + MENU_H - 8;
    int vis = (lb - lt) / MENU_ROW_H;
    if (vis < 1) vis = 1;
    if (list_top)    *list_top = lt;
    if (list_bottom) *list_bottom = lb;
    if (arrow_x)     *arrow_x = 4 + MENU_W - MENU_ARROW_W - 4;
    if (up_y)        *up_y = lt;
    if (down_y)      *down_y = lb - 18;
    return vis;
}

static void clamp_scroll(int count, int visible)
{
    int maxs = count - visible;
    if (maxs < 0) maxs = 0;
    if (menu_scroll > maxs) menu_scroll = maxs;
    if (menu_scroll < 0) menu_scroll = 0;
}

static const char *APP_NAMES[] = {
    "File Explorer", "Terminal", "Code Editor", "Browser", "Settings",
    "Flappy Bird", "Pong", "Mario", "Maze 3D", "Tetris",
    "Snake", "Breakout", "DOOM", "C Interpreter", "Python"
};
#define APP_COUNT 15

typedef struct {
    char label[40];
    int  kind;
    int  arg;
} MenuItem;

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int contains_ci(const char *haystack, const char *needle)
{
    if (!needle[0]) return 1;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (haystack[i + j] && needle[j] && to_lower(haystack[i + j]) == to_lower(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void copy_bounded(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int build_menu_items(const char *filter, MenuItem *out, int max)
{
    int n = 0;
    for (int a = 0; a < APP_COUNT && n < max; a++) {
        if (contains_ci(APP_NAMES[a], filter)) {
            copy_bounded(out[n].label, APP_NAMES[a], 40);
            out[n].kind = 0;
            out[n].arg = a;
            n++;
        }
    }

    int total = vfs_count();
    for (int i = 0; i < total && n < max; i++) {
        const VNode *v = vfs_node(i);
        if (v && !v->is_dir && contains_ci(v->name, filter)) {
            copy_bounded(out[n].label, v->name, 40);
            out[n].kind = 1;
            out[n].arg = i;
            n++;
        }
    }
    return n;
}

static void launch_app(int index)
{
    switch (index) {
    case 0:  app_launch_explorer(); break;
    case 1:  wm_create_terminal(170, 90, 520, 340); break;
    case 2:  wm_create_notepad(210, 120, 470, 320); break;
    case 3:  wm_create_browser(90, 60, 840, 620); break;
    case 4:  wm_create_settings(180, 80, 480, 470); break;
    case 5:  wm_create_game(GAME_FLAPPY,   140, 70, 520, 560); break;
    case 6:  wm_create_game(GAME_PONG,     120, 80, 660, 460); break;
    case 7:  wm_create_game(GAME_MARIO,     80, 70, 820, 470); break;
    case 8:  wm_create_game(GAME_RAYCAST,  130, 70, 680, 520); break;
    case 9:  wm_create_game(GAME_TETRIS,   220, 50, 460, 640); break;
    case 10: wm_create_game(GAME_SNAKE,    160, 60, 560, 500); break;
    case 11: wm_create_game(GAME_BREAKOUT, 150, 70, 620, 480); break;
    case 12: wm_create_doom(70, 40, 656, 452); break;
    case 13: wm_create_interp(INTERP_C,  190, 70, 560, 420); break;
    case 14: wm_create_interp(INTERP_PY, 210, 90, 560, 420); break;
    default: break;
    }
}

static void draw_text_clipped(int x, int y, const char *s, int max_cols, color_t fg, color_t bg)
{
    char clipped[64];
    if (max_cols <= 0) return;
    if (max_cols > 63) max_cols = 63;
    int i = 0;
    while (s[i] && i < max_cols) { clipped[i] = s[i]; i++; }
    clipped[i] = '\0';
    gfx_text(x, y, clipped, fg, bg);
}

static void draw_start_logo(int x, int y)
{
    color_t white = fb_rgb(255, 255, 255);
    fb_fill_rect(x,     y,     7, 7, white);
    fb_fill_rect(x + 9, y,     7, 7, white);
    fb_fill_rect(x,     y + 9, 7, 7, white);
    fb_fill_rect(x + 9, y + 9, 7, 7, white);
}

static void draw_start_menu(void)
{
    int bar_y = fb_height() - BAR_H;
    int mx = 4, my = bar_y - MENU_H;
    color_t menu_bg = fb_rgb(36, 40, 54);
    color_t box_bg  = fb_rgb(245, 245, 250);
    color_t accent  = theme_accent();

    fb_fill_rect(mx - 2, my - 2, MENU_W + 4, MENU_H + 4, fb_rgb(16, 18, 26));
    fb_fill_rect(mx, my, MENU_W, MENU_H, menu_bg);
    fb_fill_rect(mx, my, MENU_W, 30, accent);
    gfx_text(mx + 12, my + 8, "pefiaOS", fb_rgb(255, 255, 255), accent);

    int sx = mx + 10, sy = my + 42, sw = MENU_W - 20, sh = 22;
    fb_fill_rect(sx, sy, sw, sh, box_bg);
    char shown[40];
    copy_bounded(shown, search_len == 0 ? "Search..." : search_text, 40);
    draw_text_clipped(sx + 6, sy + 3, shown, (sw - 12) / 8, fb_rgb(60, 60, 70), box_bg);

    MenuItem items[128];
    int count = build_menu_items(search_text, items, 128);

    int list_top, list_bottom, arrow_x, up_y, down_y;
    int visible = menu_geom(my, &list_top, &list_bottom, &arrow_x, &up_y, &down_y);
    int scrollable = count > visible;
    clamp_scroll(count, visible);

    int text_cols = ((scrollable ? MENU_W - MENU_ARROW_W - 6 : MENU_W) - 44) / 8;
    for (int r = 0; r < visible; r++) {
        int i = menu_scroll + r;
        if (i >= count) break;
        int row_y = list_top + r * MENU_ROW_H;
        color_t bullet = items[i].kind == 0 ? accent : fb_rgb(150, 156, 168);
        fb_fill_rect(mx + 12, row_y + 3, 14, 14, bullet);
        draw_text_clipped(mx + 34, row_y + 4, items[i].label, text_cols,
                           fb_rgb(235, 235, 240), menu_bg);
    }

    if (scrollable) {
        color_t up_c   = menu_scroll > 0            ? fb_rgb(220, 224, 232) : fb_rgb(96, 100, 112);
        color_t down_c = menu_scroll < count - visible ? fb_rgb(220, 224, 232) : fb_rgb(96, 100, 112);
        fb_fill_rect(arrow_x, up_y,   MENU_ARROW_W, 18, fb_rgb(52, 58, 76));
        fb_fill_rect(arrow_x, down_y, MENU_ARROW_W, 18, fb_rgb(52, 58, 76));
        gfx_text(arrow_x + 4, up_y   + 1, "^", up_c,   fb_rgb(52, 58, 76));
        gfx_text(arrow_x + 4, down_y + 1, "v", down_c, fb_rgb(52, 58, 76));
    }
}

void taskbar_paint(void)
{
    int screen_w = fb_width(), screen_h = fb_height();
    int bar_y = screen_h - BAR_H;
    color_t bar_bg = fb_rgb(20, 24, 36);

    uint8_t ar, ag, ab; theme_accent_rgb(&ar, &ag, &ab);
    color_t accent     = fb_rgb(ar, ag, ab);
    color_t accent_dim = fb_rgb((uint8_t)(ar * 4 / 5), (uint8_t)(ag * 4 / 5), (uint8_t)(ab * 4 / 5));

    fb_fill_rect(0, bar_y, screen_w, BAR_H, bar_bg);
    fb_fill_rect(0, bar_y, screen_w, 2, accent);

    color_t start_bg = menu_open ? accent_dim : accent;
    fb_fill_rect(0, bar_y, START_W, BAR_H, start_bg);
    draw_start_logo(14, bar_y + 12);
    gfx_text(40, bar_y + 12, "Start", fb_rgb(255, 255, 255), start_bg);

    int win_count = wm_count();
    for (int i = 0; i < win_count; i++) {
        int bx = START_W + NET_W + 8 + i * (BTN_W + BTN_GAP);
        if (bx + BTN_W > screen_w - 70) break;
        fb_fill_rect(bx, bar_y + 6, BTN_W, BAR_H - 12, fb_rgb(48, 54, 72));
        draw_text_clipped(bx + 8, bar_y + 12, wm_title(i), (BTN_W - 14) / 8,
                           fb_rgb(235, 235, 245), fb_rgb(48, 54, 72));
    }

    int net_x = START_W;
    color_t net_bg = fb_rgb(30, 38, 56);
    fb_fill_rect(net_x, bar_y, NET_W, BAR_H, net_bg);
    /* four bars of a signal-strength glyph, ascending height */
    fb_fill_rect(net_x + 10, bar_y + 10, 3, 16, fb_rgb(180, 188, 208));
    fb_fill_rect(net_x + 14, bar_y + 13, 3, 13, fb_rgb(160, 188, 228));
    fb_fill_rect(net_x + 18, bar_y + 16, 3, 10, fb_rgb(130, 174, 238));
    fb_fill_rect(net_x + 22, bar_y + 19, 3, 7,  fb_rgb(96, 160, 245));

    const char *status = net_status_text();
    const char *tag = (status && status[0]) ? "WiFi" : "Net";
    color_t tag_fg = net_ready() ? fb_rgb(144, 232, 160) : fb_rgb(230, 170, 120);
    draw_text_clipped(net_x + 30, bar_y + 12, tag, 10, tag_fg, net_bg);

    int h, m, s;
    rtc_time(&h, &m, &s);
    char clock_str[6];
    clock_str[0] = (char)('0' + (h / 10) % 10);
    clock_str[1] = (char)('0' + h % 10);
    clock_str[2] = ':';
    clock_str[3] = (char)('0' + (m / 10) % 10);
    clock_str[4] = (char)('0' + m % 10);
    clock_str[5] = '\0';
    gfx_text(screen_w - 52, bar_y + 12, clock_str, fb_rgb(235, 235, 245), bar_bg);

    if (menu_open) draw_start_menu();
}

int taskbar_menu_open(void) { return menu_open; }

void taskbar_key(char c)
{
    if (!menu_open) return;
    if (c == '\b') {
        if (search_len > 0) search_text[--search_len] = '\0';
    } else if (c >= 32 && c < 127 && search_len < (int)sizeof(search_text) - 1) {
        search_text[search_len++] = c;
        search_text[search_len] = '\0';
    }
    menu_scroll = 0;   /* filter changed: jump back to the top of the results */
}

int taskbar_click(int mx, int my)
{
    int screen_w = fb_width(), screen_h = fb_height();
    int bar_y = screen_h - BAR_H;

    if (my >= bar_y && mx < START_W) { menu_open = !menu_open; if (menu_open) menu_scroll = 0; return 1; }

    if (menu_open) {
        int menu_x = 4, menu_y = bar_y - MENU_H;
        if (mx >= menu_x && mx < menu_x + MENU_W && my >= menu_y && my < menu_y + MENU_H) {
            MenuItem items[128];
            int count = build_menu_items(search_text, items, 128);

            int list_top, list_bottom, arrow_x, up_y, down_y;
            int visible = menu_geom(menu_y, &list_top, &list_bottom, &arrow_x, &up_y, &down_y);
            clamp_scroll(count, visible);
            int scrollable = count > visible;

            if (scrollable && mx >= arrow_x && mx < arrow_x + MENU_ARROW_W) {
                if (my >= up_y   && my < up_y   + 18) { menu_scroll -= 3; clamp_scroll(count, visible); return 1; }
                if (my >= down_y && my < down_y + 18) { menu_scroll += 3; clamp_scroll(count, visible); return 1; }
            }

            for (int r = 0; r < visible; r++) {
                int i = menu_scroll + r;
                if (i >= count) break;
                int row_y = list_top + r * MENU_ROW_H;
                if (my >= row_y && my < row_y + MENU_ROW_H) {
                    if (items[i].kind == 0) {
                        launch_app(items[i].arg);
                    } else {
                        const VNode *f = vfs_node(items[i].arg);
                        int dir = (f && f->parent >= 0) ? f->parent : vfs_root();
                        wm_create_explorer(120, 80, 360, 300, "File Explorer", dir);
                    }
                    menu_open = 0;
                    return 1;
                }
            }
            return 1;
        }
        menu_open = 0;
        return 1;
    }

    if (my >= bar_y) {
        int win_count = wm_count();
        for (int i = 0; i < win_count; i++) {
            int bx = START_W + NET_W + 8 + i * (BTN_W + BTN_GAP);
            if (bx + BTN_W > screen_w - 70) break;
            if (mx >= bx && mx < bx + BTN_W) { wm_raise(i); return 1; }
        }
        return 1;
    }
    return 0;
}
