/* kernel/taskbar.c
 * Desktop shell chrome: bottom bar (Start button with logo, a button per open
 * window, live clock) and a Start menu with a search box that filters apps and
 * files. Launches File Explorer, Terminal, and Notepad.
 */
#include "taskbar.h"
#include "wm.h"
#include "explorer.h"
#include "vfs.h"
#include "rtc.h"
#include "framebuffer.h"
#include "console.h"
#include "net.h"
#include "games.h"

#define BAR_H    40
#define START_W  92
#define NET_W    120
#define MENU_W   300
#define MENU_H   400
#define BTN_W    156
#define BTN_GAP  4

static int  menu_open = 0;
static char search[40];
static int  searchlen = 0;

static const char *APPS[] = {
    "File Explorer", "Terminal", "Notepad", "Browser",
    "Flappy Bird", "Pong", "Mario", "Maze 3D", "Tetris",
    "Snake", "Breakout", "DOOM"
};
#define NAPPS 12

typedef struct { char label[40]; int kind; int arg; } Item;  /* kind 0=app, 1=file */

static char tolow(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (hay[i + j] && needle[j] && tolow(hay[i + j]) == tolow(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void put(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int build_items(const char *filter, Item *out, int max)
{
    int n = 0;
    for (int a = 0; a < NAPPS && n < max; a++)
        if (ci_contains(APPS[a], filter)) { put(out[n].label, APPS[a], 40); out[n].kind = 0; out[n].arg = a; n++; }

    int total = vfs_count();
    for (int i = 0; i < total && n < max; i++) {
        const VNode *v = vfs_node(i);
        if (v && !v->is_dir && ci_contains(v->name, filter)) {
            put(out[n].label, v->name, 40); out[n].kind = 1; out[n].arg = i; n++;
        }
    }
    return n;
}

static void launch_app(int a)
{
    if (a == 0) app_launch_explorer();
    else if (a == 1) wm_create_terminal(170, 90, 520, 340);
    else if (a == 2) wm_create_notepad(210, 120, 470, 320);
    else if (a == 3) wm_create_browser(90, 60, 840, 620);
    else if (a == 4) wm_create_game(GAME_FLAPPY,  140, 70, 520, 560);
    else if (a == 5) wm_create_game(GAME_PONG,    120, 80, 660, 460);
    else if (a == 6) wm_create_game(GAME_MARIO,    80, 70, 820, 470);
    else if (a == 7) wm_create_game(GAME_RAYCAST, 130, 70, 680, 520);
    else if (a == 8)  wm_create_game(GAME_TETRIS,   220, 50, 460, 640);
    else if (a == 9)  wm_create_game(GAME_SNAKE,    160, 60, 560, 500);
    else if (a == 10) wm_create_game(GAME_BREAKOUT, 150, 70, 620, 480);
    else if (a == 11) wm_create_doom(70, 40, 656, 452);
}

static void draw_clip(int x, int y, const char *s, int maxcols, color_t fg, color_t bg)
{
    char buf[64];
    if (maxcols <= 0) return;
    if (maxcols > 63) maxcols = 63;
    int i = 0;
    while (s[i] && i < maxcols) { buf[i] = s[i]; i++; }
    buf[i] = '\0';
    gfx_text(x, y, buf, fg, bg);
}

static void draw_start_logo(int x, int y)
{
    color_t q = fb_rgb(255, 255, 255);
    fb_fill_rect(x, y, 7, 7, q);
    fb_fill_rect(x + 9, y, 7, 7, q);
    fb_fill_rect(x, y + 9, 7, 7, q);
    fb_fill_rect(x + 9, y + 9, 7, 7, q);
}

static void draw_menu(void)
{
    int barY = fb_height() - BAR_H;
    int mx = 4, my = barY - MENU_H;
    color_t mbg = fb_rgb(36, 40, 54);
    color_t box = fb_rgb(245, 245, 250);
    color_t accent = fb_rgb(0, 120, 215);

    fb_fill_rect(mx - 2, my - 2, MENU_W + 4, MENU_H + 4, fb_rgb(16, 18, 26));
    fb_fill_rect(mx, my, MENU_W, MENU_H, mbg);
    fb_fill_rect(mx, my, MENU_W, 30, accent);
    gfx_text(mx + 12, my + 8, "pefiaOS", fb_rgb(255, 255, 255), accent);

    int sx = mx + 10, sy = my + 42, sw = MENU_W - 20, sh = 22;
    fb_fill_rect(sx, sy, sw, sh, box);
    char shown[40];
    if (searchlen == 0) put(shown, "Search...", 40); else put(shown, search, 40);
    draw_clip(sx + 6, sy + 3, shown, (sw - 12) / 8, fb_rgb(60, 60, 70), box);

    Item items[64];
    int cnt = build_items(search, items, 64);
    int ly = my + 80;
    for (int i = 0; i < cnt; i++) {
        int ry = ly + i * 24;
        if (ry + 24 > my + MENU_H) break;
        color_t ic = items[i].kind == 0 ? accent : fb_rgb(150, 156, 168);
        fb_fill_rect(mx + 12, ry + 3, 14, 14, ic);
        draw_clip(mx + 34, ry + 4, items[i].label, (MENU_W - 44) / 8, fb_rgb(235, 235, 240), mbg);
    }
}

void taskbar_paint(void)
{
    int W = fb_width(), H = fb_height();
    int barY = H - BAR_H;
    color_t bar = fb_rgb(20, 24, 36);

    fb_fill_rect(0, barY, W, BAR_H, bar);
    fb_fill_rect(0, barY, W, 2, fb_rgb(0, 120, 215));     /* accent top edge */

    color_t sb = menu_open ? fb_rgb(0, 100, 190) : fb_rgb(0, 120, 215);
    fb_fill_rect(0, barY, START_W, BAR_H, sb);
    draw_start_logo(14, barY + 12);
    gfx_text(40, barY + 12, "Start", fb_rgb(255, 255, 255), sb);

    int n = wm_count();
    for (int i = 0; i < n; i++) {
        int bx = START_W + NET_W + 8 + i * (BTN_W + BTN_GAP);
        if (bx + BTN_W > W - 70) break;
        fb_fill_rect(bx, barY + 6, BTN_W, BAR_H - 12, fb_rgb(48, 54, 72));
        draw_clip(bx + 8, barY + 12, wm_title(i), (BTN_W - 14) / 8,
                  fb_rgb(235, 235, 245), fb_rgb(48, 54, 72));
    }

    int nx = START_W;
    color_t nbg = fb_rgb(30, 38, 56);
    fb_fill_rect(nx, barY, NET_W, BAR_H, nbg);
    fb_fill_rect(nx + 10, barY + 10, 3, 16, fb_rgb(180, 188, 208));
    fb_fill_rect(nx + 14, barY + 13, 3, 13, fb_rgb(160, 188, 228));
    fb_fill_rect(nx + 18, barY + 16, 3, 10, fb_rgb(130, 174, 238));
    fb_fill_rect(nx + 22, barY + 19, 3, 7,  fb_rgb(96, 160, 245));

    {
        const char *ns = net_status_text();
        const char *tag = (ns && ns[0]) ? "WiFi" : "Net";
        color_t tfg = net_ready() ? fb_rgb(144, 232, 160) : fb_rgb(230, 170, 120);
        draw_clip(nx + 30, barY + 12, tag, 10, tfg, nbg);
    }

    int h, m, s;
    rtc_time(&h, &m, &s);
    char clk[6];
    clk[0] = (char)('0' + (h / 10) % 10); clk[1] = (char)('0' + h % 10); clk[2] = ':';
    clk[3] = (char)('0' + (m / 10) % 10); clk[4] = (char)('0' + m % 10); clk[5] = '\0';
    gfx_text(W - 52, barY + 12, clk, fb_rgb(235, 235, 245), bar);

    if (menu_open) draw_menu();
}

int taskbar_menu_open(void) { return menu_open; }

void taskbar_key(char c)
{
    if (!menu_open) return;
    if (c == '\b') { if (searchlen > 0) search[--searchlen] = '\0'; }
    else if (c >= 32 && c < 127 && searchlen < (int)sizeof(search) - 1) {
        search[searchlen++] = c;
        search[searchlen] = '\0';
    }
}

int taskbar_click(int mx, int my)
{
    int W = fb_width(), H = fb_height();
    int barY = H - BAR_H;

    if (my >= barY && mx < START_W) { menu_open = !menu_open; return 1; }

    if (menu_open) {
        int menuX = 4, menuY = barY - MENU_H;
        if (mx >= menuX && mx < menuX + MENU_W && my >= menuY && my < menuY + MENU_H) {
            Item items[64];
            int cnt = build_items(search, items, 64);
            int ly = menuY + 80;
            for (int i = 0; i < cnt; i++) {
                int ry = ly + i * 24;
                if (ry + 24 > menuY + MENU_H) break;
                if (my >= ry && my < ry + 24) {
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

    if (my >= barY) {
        int n = wm_count();
        for (int i = 0; i < n; i++) {
            int bx = START_W + NET_W + 8 + i * (BTN_W + BTN_GAP);
            if (bx + BTN_W > W - 70) break;
            if (mx >= bx && mx < bx + BTN_W) { wm_raise(i); return 1; }
        }
        return 1;
    }
    return 0;
}
