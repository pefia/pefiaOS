#include "settings.h"
#include "framebuffer.h"
#include "console.h"
#include "theme.h"
#include "rtc.h"
#include "net.h"

#define PAD 16

typedef struct { int x, y, w, h; } Rect;

typedef struct {
    Rect swatch;
    Rect slider[3];
    Rect dark;
    Rect hminus, hplus, mminus, mplus;
    Rect reconnect;
    int  head_appear_y, head_time_y, head_net_y;
    int  clock_y, net_y;
} Layout;

/* All rects in window-local coords (0,0 = top-left of the content area). */
static void compute_layout(int wdt, Layout *L)
{
    int cx = PAD;
    int slider_x = PAD + 84;
    int slider_w = wdt - slider_x - PAD - 40;
    if (slider_w < 60) slider_w = 60;
    int y = PAD;

    L->head_appear_y = y;  y += 26;
    L->swatch = (Rect){ cx, y, 52, 52 };
    L->slider[0] = (Rect){ slider_x, y +  2, slider_w, 14 };
    L->slider[1] = (Rect){ slider_x, y + 20, slider_w, 14 };
    L->slider[2] = (Rect){ slider_x, y + 38, slider_w, 14 };
    y += 60;
    L->dark = (Rect){ cx, y, 132, 26 };
    y += 44;

    L->head_time_y = y;   y += 26;
    L->clock_y = y;       y += 26;
    L->hminus = (Rect){ cx,        y, 40, 26 };
    L->hplus  = (Rect){ cx + 44,   y, 40, 26 };
    L->mminus = (Rect){ cx + 104,  y, 40, 26 };
    L->mplus  = (Rect){ cx + 148,  y, 40, 26 };
    y += 44;

    L->head_net_y = y;    y += 26;
    L->net_y = y;         y += 46;
    L->reconnect = (Rect){ cx, y, 118, 26 };
}

static int u2s(unsigned v, char *out)
{
    char tmp[12]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static void two(int v, char *out) { out[0] = (char)('0' + (v / 10) % 10); out[1] = (char)('0' + v % 10); out[2] = 0; }

static void fill(int x, int y, const Rect *r, color_t c) { fb_fill_rect(x + r->x, y + r->y, r->w, r->h, c); }

static void button(int x, int y, const Rect *r, const char *label, color_t bg, color_t fg)
{
    fb_fill_rect(x + r->x, y + r->y, r->w, r->h, bg);
    fb_fill_rect(x + r->x, y + r->y + r->h - 1, r->w, 1, fb_rgb(120, 128, 144));
    int lw = 0; while (label[lw]) lw++;
    gfx_text(x + r->x + (r->w - lw * 8) / 2, y + r->y + (r->h - 16) / 2, label, fg, bg);
}

static void header(int x, int y_line, const char *s, color_t accent, color_t panel)
{
    gfx_text(x + PAD, y_line, s, accent, panel);
    fb_fill_rect(x + PAD, y_line + 18, 220, 1, fb_rgb(200, 206, 216));
}

void settings_paint(Window *w, int x, int y, int wdt, int hgt)
{
    (void)w;
    Layout L;
    compute_layout(wdt, &L);

    color_t panel  = theme_dark() ? fb_rgb(238, 241, 246) : fb_rgb(246, 248, 251);
    color_t text   = fb_rgb(40, 46, 58);
    color_t accent = theme_accent();
    fb_fill_rect(x, y, wdt, hgt, panel);

    header(x, y + L.head_appear_y, "Appearance", accent, panel);

    uint8_t ar, ag, ab; theme_accent_rgb(&ar, &ag, &ab);
    fill(x, y, &L.swatch, accent);
    fb_fill_rect(x + L.swatch.x, y + L.swatch.y, L.swatch.w, 1, fb_rgb(150, 156, 168));

    const char  chan[3]   = { 'R', 'G', 'B' };
    const int   val[3]    = { ar, ag, ab };
    const color_t track[3] = { fb_rgb(210, 120, 120), fb_rgb(120, 200, 130), fb_rgb(120, 150, 220) };
    for (int i = 0; i < 3; i++) {
        Rect *t = &L.slider[i];
        char c[2] = { chan[i], 0 };
        gfx_text(x + PAD, y + t->y - 1, c, text, panel);
        fb_fill_rect(x + t->x, y + t->y, t->w, t->h, fb_rgb(214, 218, 226));
        int fillw = val[i] * t->w / 255;
        fb_fill_rect(x + t->x, y + t->y, fillw, t->h, track[i]);

        fb_fill_rect(x + t->x + fillw - 2, y + t->y - 2, 4, t->h + 4, fb_rgb(60, 66, 78));
        char num[6]; u2s((unsigned)val[i], num);
        gfx_text(x + t->x + t->w + 8, y + t->y - 1, num, text, panel);
    }

    button(x, y, &L.dark, theme_dark() ? "Theme: Dark" : "Theme: Light",
           fb_rgb(224, 228, 236), text);

    header(x, y + L.head_time_y, "Date & Time", accent, panel);
    int h, m, s; rtc_time(&h, &m, &s);
    char clock[9], hh[3], mm[3], ss[3];
    two(h, hh); two(m, mm); two(s, ss);
    clock[0]=hh[0]; clock[1]=hh[1]; clock[2]=':'; clock[3]=mm[0]; clock[4]=mm[1];
    clock[5]=':'; clock[6]=ss[0]; clock[7]=ss[1]; clock[8]=0;
    gfx_text(x + PAD, y + L.clock_y, clock, text, panel);
    button(x, y, &L.hminus, "H-", fb_rgb(224, 228, 236), text);
    button(x, y, &L.hplus,  "H+", fb_rgb(224, 228, 236), text);
    button(x, y, &L.mminus, "M-", fb_rgb(224, 228, 236), text);
    button(x, y, &L.mplus,  "M+", fb_rgb(224, 228, 236), text);

    header(x, y + L.head_net_y, "Network", accent, panel);
    int ready = net_ready();
    gfx_text(x + PAD, y + L.net_y, ready ? "Status: Connected" : "Status: Offline",
             ready ? fb_rgb(30, 140, 70) : fb_rgb(190, 90, 40), panel);
    uint32_t ip = net_local_ip();
    char ipline[40]; int p = 0;
    const char *pre = "IP: "; while (*pre) ipline[p++] = *pre++;
    p += u2s((ip >> 24) & 0xFF, ipline + p); ipline[p++] = '.';
    p += u2s((ip >> 16) & 0xFF, ipline + p); ipline[p++] = '.';
    p += u2s((ip >>  8) & 0xFF, ipline + p); ipline[p++] = '.';
    p += u2s( ip        & 0xFF, ipline + p); ipline[p] = 0;
    gfx_text(x + PAD, y + L.net_y + 20, ipline, text, panel);
    button(x, y, &L.reconnect, "Reconnect", theme_accent(), fb_rgb(255, 255, 255));
}

static int hit(const Rect *r, int rx, int ry)
{
    return rx >= r->x && rx < r->x + r->w && ry >= r->y && ry < r->y + r->h;
}

void settings_click(Window *w, int relx, int rely)
{
    Layout L;
    compute_layout(w->w, &L);

    uint8_t ar, ag, ab; theme_accent_rgb(&ar, &ag, &ab);
    int rgb[3] = { ar, ag, ab };
    for (int i = 0; i < 3; i++) {
        if (hit(&L.slider[i], relx, rely)) {
            int v = (relx - L.slider[i].x) * 255 / (L.slider[i].w > 0 ? L.slider[i].w : 1);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            rgb[i] = v;
            theme_set_accent(rgb[0], rgb[1], rgb[2]);
            return;
        }
    }

    if (hit(&L.dark, relx, rely)) { theme_set_dark(!theme_dark()); return; }

    int h, m, s; rtc_time(&h, &m, &s);
    if (hit(&L.hminus, relx, rely)) { rtc_set((h + 23) % 24, m, s); return; }
    if (hit(&L.hplus,  relx, rely)) { rtc_set((h +  1) % 24, m, s); return; }
    if (hit(&L.mminus, relx, rely)) { rtc_set(h, (m + 59) % 60, s); return; }
    if (hit(&L.mplus,  relx, rely)) { rtc_set(h, (m +  1) % 60, s); return; }

    if (hit(&L.reconnect, relx, rely)) { net_init(); return; }
    (void)w;
}
