/* kernel/games.c
 * -----------------------------------------------------------------------------
 * Implementation of the five built-in games (see games.h).
 *
 * Rendering model: every game owns an off-screen buffer the size of its window
 * content area. A frame is composed entirely into that buffer, then presented
 * in a single fb_blit() so animation never flickers. The window manager calls
 * game_tick() once per main-loop iteration; each game self-paces against
 * clock_ms() so motion is the same speed regardless of how fast the loop spins.
 *
 * Controls come from the real-time key-state API (input.h): arrow keys or WASD
 * for movement, space for the action button (flap / jump / hard-drop / restart).
 * -----------------------------------------------------------------------------
 */
#include "games.h"
#include "wm.h"
#include "framebuffer.h"
#include "console.h"
#include "input.h"
#include "mouse.h"
#include "heap.h"
#include "clock.h"
#include "util.h"

#include <stdint.h>
#include <stddef.h>

/* ===========================================================================
 * Shared state
 * ======================================================================== */

#define MAX_MW 64
#define MAX_MH 16
#define TS     24          /* Mario tile size in pixels */
#define PW     18          /* Mario player width  */
#define PH     22          /* Mario player height */
#define ES     20          /* Mario enemy size    */
#define COLS   10          /* Tetris board width  */
#define ROWS   18          /* Tetris board height */

typedef struct {
    int       kind;
    color_t  *fbuf;        /* off-screen frame, cw*ch native pixels */
    int       cw, ch;      /* current buffer dimensions             */
    int       inited;      /* game state has been reset once        */
    uint32_t  last_ms;     /* time of last logic step               */
    int       over, won, score, best;

    /* --- Flappy --- */
    int f_y, f_vy, f_pw, f_spacing, f_px[3], f_gap[3];

    /* --- Pong --- */
    int p_lpy, p_rpy, p_bx, p_by, p_bvx, p_bvy, p_ls, p_rs, p_ph, p_pw, p_bs;

    /* --- Mario --- */
    int  m_px, m_py, m_vx, m_vy, m_on, m_cam, m_lives, m_W, m_H, m_sx, m_sy;
    char m_map[MAX_MW * MAX_MH];
    int  e_x[8], e_y[8], e_vx[8], e_lo[8], e_hi[8], e_on[8], m_ne;

    /* --- Raycaster --- */
    int r_px, r_py, r_ang;

    /* --- Tetris --- */
    uint8_t t_board[ROWS * COLS];
    int     t_type, t_rot, t_x, t_y, t_acc, t_lines;
} Game;

/* ===========================================================================
 * Tiny PRNG + integer sine table (no libm in a freestanding kernel)
 * ======================================================================== */

static uint32_t rng_state = 0x2545F491u;
static int      mod_ready  = 0;
static int      sintab[360];           /* sin(deg) scaled by 1024 */

static uint32_t grand(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

static int grange(int lo, int hi)      /* [lo, hi) */
{
    if (hi <= lo) return lo;
    return lo + (int)(grand() % (uint32_t)(hi - lo));
}

/* Bhaskara I's degree-form sine approximation; mirrored for the lower half. */
static void module_init(void)
{
    if (mod_ready) return;
    rng_state ^= (uint32_t)rdtsc() | 1u;
    for (int d = 0; d <= 180; d++) {
        int xd  = d * (180 - d);                 /* 0..8100 */
        int den = 40500 - xd;                    /* never zero on this range */
        sintab[d] = (4 * xd * 1024) / den;
    }
    for (int d = 181; d < 360; d++) sintab[d] = -sintab[d - 180];
    mod_ready = 1;
}

static int sind(int a) { a %= 360; if (a < 0) a += 360; return sintab[a]; }
static int cosd(int a) { return sind(a + 90); }

/* ===========================================================================
 * Off-screen drawing primitives (operate on g->fbuf)
 * ======================================================================== */

static inline void gpx(Game *g, int x, int y, color_t c)
{
    if ((unsigned)x < (unsigned)g->cw && (unsigned)y < (unsigned)g->ch)
        g->fbuf[(long)y * g->cw + x] = c;
}

static void gfill(Game *g, int x, int y, int w, int h, color_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g->cw) w = g->cw - x;
    if (y + h > g->ch) h = g->ch - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        color_t *row = g->fbuf + (long)(y + j) * g->cw + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void gclear(Game *g, color_t c) { gfill(g, 0, 0, g->cw, g->ch, c); }

/* shade a base colour by 0..255 (for distance fog in the raycaster) */
static color_t shade(int r, int gg, int b, int s)
{
    if (s < 0) s = 0;
    if (s > 255) s = 255;
    return fb_rgb((uint8_t)(r * s / 255), (uint8_t)(gg * s / 255), (uint8_t)(b * s / 255));
}

/* ===========================================================================
 * Control helpers (held vs. just-pressed)
 * ======================================================================== */

static int left_held (void) { return input_key_down(IK_LEFT)  || input_key_down(IK_A); }
static int right_held(void) { return input_key_down(IK_RIGHT) || input_key_down(IK_D); }
static int up_held   (void) { return input_key_down(IK_UP)    || input_key_down(IK_W); }
static int down_held (void) { return input_key_down(IK_DOWN)  || input_key_down(IK_S); }

static int left_press (void) { return input_key_pressed(IK_LEFT)  || input_key_pressed(IK_A); }
static int right_press(void) { return input_key_pressed(IK_RIGHT) || input_key_pressed(IK_D); }
static int up_press   (void) { return input_key_pressed(IK_UP)    || input_key_pressed(IK_W); }
static int space_press(void) { return input_key_pressed(IK_SPACE); }
/* "action" used for flap/jump/restart: space or up/W */
static int act_press  (void) { return input_key_pressed(IK_SPACE) ||
                                      input_key_pressed(IK_UP) || input_key_pressed(IK_W); }

/* ===========================================================================
 * Text overlay helpers (drawn on-screen, after the blit)
 * ======================================================================== */

static void label(int x, int y, const char *s, color_t fg, color_t bg)
{
    int w = (int)kstrlen(s) * 8 + 8;
    fb_fill_rect(x - 4, y - 3, w, 22, bg);
    gfx_text(x, y, s, fg, bg);
}

static void center_msg(int cx, int cy, int cw, int ch, const char *s, color_t fg, color_t bg)
{
    int len = (int)kstrlen(s);
    int x = cx + (cw - len * 8) / 2;
    int y = cy + ch / 2 - 8;
    label(x, y, s, fg, bg);
}

static void numlabel(char *dst, const char *pre, int v)
{
    char n[12];
    kstrcpy(dst, pre);
    kutoa((uint32_t)v, n);
    kstrcat(dst, n);
}

/* ===========================================================================
 * FLAPPY BIRD
 * ======================================================================== */

static void flappy_reset(Game *g)
{
    g->over = 0; g->score = 0;
    g->f_y = (g->ch / 2) << 6; g->f_vy = 0;
    g->f_pw = g->cw / 10; if (g->f_pw < 30) g->f_pw = 30;
    g->f_spacing = g->cw / 2; if (g->f_spacing < g->f_pw * 3) g->f_spacing = g->f_pw * 3;
    int gaph = g->ch / 6; if (gaph < 44) gaph = 44;
    for (int i = 0; i < 3; i++) {
        g->f_px[i]  = g->cw + 40 + i * g->f_spacing;
        g->f_gap[i] = grange(gaph + 12, g->ch - gaph - 12);
    }
}

static void flappy_step(Game *g)
{
    int gaph = g->ch / 6; if (gaph < 44) gaph = 44;
    if (g->over) { if (act_press()) flappy_reset(g); return; }

    int grav = (g->ch << 6) / 1400; if (grav < 1) grav = 1;
    int flap = -(g->ch << 6) / 52;
    int maxv = (g->ch << 6) / 30;

    if (act_press()) g->f_vy = flap;
    g->f_vy += grav;
    if (g->f_vy > maxv) g->f_vy = maxv;
    g->f_y += g->f_vy;

    int br = g->cw / 26; if (br < 8) br = 8;
    int bx = g->cw / 4;
    int by = g->f_y >> 6;

    if (by - br < 0) { g->f_y = br << 6; g->f_vy = 0; by = br; }
    if (by + br >= g->ch) { g->over = 1; if (g->score > g->best) g->best = g->score; return; }

    int speed = g->cw / 180 + 2;
    for (int i = 0; i < 3; i++) {
        g->f_px[i] -= speed;
        if (g->f_px[i] < -g->f_pw) {
            int mx = 0;
            for (int k = 0; k < 3; k++) if (g->f_px[k] > mx) mx = g->f_px[k];
            g->f_px[i]  = mx + g->f_spacing;
            g->f_gap[i] = grange(gaph + 12, g->ch - gaph - 12);
            g->score++;
        }
        if (bx + br > g->f_px[i] && bx - br < g->f_px[i] + g->f_pw) {
            if (by - br < g->f_gap[i] - gaph || by + br > g->f_gap[i] + gaph) {
                g->over = 1; if (g->score > g->best) g->best = g->score;
            }
        }
    }
}

static void flappy_draw(Game *g)
{
    gclear(g, fb_rgb(112, 197, 232));
    int ground = g->ch - g->ch / 8;
    gfill(g, 0, ground, g->cw, g->ch - ground, fb_rgb(222, 196, 120));
    gfill(g, 0, ground, g->cw, 5, fb_rgb(118, 184, 84));

    int gaph = g->ch / 6; if (gaph < 44) gaph = 44;
    color_t pipe = fb_rgb(78, 178, 82), lip = fb_rgb(58, 148, 64);
    for (int i = 0; i < 3; i++) {
        int px = g->f_px[i], gy = g->f_gap[i];
        gfill(g, px, 0, g->f_pw, gy - gaph, pipe);
        gfill(g, px, gy + gaph, g->f_pw, g->ch - (gy + gaph), pipe);
        gfill(g, px - 3, gy - gaph - 14, g->f_pw + 6, 14, lip);
        gfill(g, px - 3, gy + gaph, g->f_pw + 6, 14, lip);
    }

    int br = g->cw / 26; if (br < 8) br = 8;
    int bx = g->cw / 4, by = g->f_y >> 6;
    gfill(g, bx - br, by - br, 2 * br, 2 * br, fb_rgb(248, 216, 72));      /* body  */
    gfill(g, bx - br, by + br - 4, 2 * br, 4, fb_rgb(228, 170, 60));        /* belly */
    gfill(g, bx + br - 6, by - 5, 6, 6, fb_rgb(255, 255, 255));            /* eye   */
    gfill(g, bx + br - 3, by - 4, 3, 3, fb_rgb(20, 20, 20));              /* pupil */
    gfill(g, bx + br, by + 1, 6, 4, fb_rgb(232, 132, 40));               /* beak  */
}

static void flappy_overlay(Game *g, int cx, int cy)
{
    char b[40];
    numlabel(b, "Score ", g->score);
    label(cx + 8, cy + 6, b, fb_rgb(255, 255, 255), fb_rgb(24, 64, 96));
    if (g->over) {
        numlabel(b, "GAME OVER  -  SPACE   best ", g->best);
        center_msg(cx, cy, g->cw, g->ch, b, fb_rgb(255, 232, 130), fb_rgb(120, 36, 44));
    }
}

/* ===========================================================================
 * PONG
 * ======================================================================== */

static void pong_serve(Game *g, int dir)
{
    g->p_bx = g->cw / 2; g->p_by = g->ch / 2;
    int sp = g->cw / 130 + 3;
    g->p_bvx = dir ? sp : -sp;
    g->p_bvy = (grand() & 1) ? (sp / 2 + 1) : -(sp / 2 + 1);
}

static void pong_reset(Game *g)
{
    g->p_pw = g->cw / 44; if (g->p_pw < 8)  g->p_pw = 8;
    g->p_ph = g->ch / 4;  if (g->p_ph < 44) g->p_ph = 44;
    g->p_bs = g->cw / 70; if (g->p_bs < 8)  g->p_bs = 8;
    g->p_lpy = (g->ch - g->p_ph) / 2;
    g->p_rpy = g->p_lpy;
    g->p_ls = g->p_rs = 0;
    pong_serve(g, grand() & 1);
}

static void pong_step(Game *g)
{
    int psp = g->ch / 40 + 5;
    if (up_held())   g->p_lpy -= psp;
    if (down_held()) g->p_lpy += psp;
    if (g->p_lpy < 0) g->p_lpy = 0;
    if (g->p_lpy > g->ch - g->p_ph) g->p_lpy = g->ch - g->p_ph;

    int ai = g->ch / 60 + 3;                       /* CPU paddle, slightly beatable */
    int aimc = g->p_rpy + g->p_ph / 2;
    if (aimc < g->p_by - 6) g->p_rpy += ai;
    else if (aimc > g->p_by + 6) g->p_rpy -= ai;
    if (g->p_rpy < 0) g->p_rpy = 0;
    if (g->p_rpy > g->ch - g->p_ph) g->p_rpy = g->ch - g->p_ph;

    g->p_bx += g->p_bvx; g->p_by += g->p_bvy;
    if (g->p_by < 0) { g->p_by = 0; g->p_bvy = -g->p_bvy; }
    if (g->p_by + g->p_bs > g->ch) { g->p_by = g->ch - g->p_bs; g->p_bvy = -g->p_bvy; }

    int lpx = 14, rpx = g->cw - 14 - g->p_pw;
    if (g->p_bvx < 0 && g->p_bx <= lpx + g->p_pw && g->p_bx >= lpx - g->p_bs &&
        g->p_by + g->p_bs >= g->p_lpy && g->p_by <= g->p_lpy + g->p_ph) {
        g->p_bx = lpx + g->p_pw; g->p_bvx = -g->p_bvx;
        g->p_bvy += ((g->p_by + g->p_bs / 2) - (g->p_lpy + g->p_ph / 2)) / 8;
    }
    if (g->p_bvx > 0 && g->p_bx + g->p_bs >= rpx && g->p_bx + g->p_bs <= rpx + g->p_pw + 6 &&
        g->p_by + g->p_bs >= g->p_rpy && g->p_by <= g->p_rpy + g->p_ph) {
        g->p_bx = rpx - g->p_bs; g->p_bvx = -g->p_bvx;
        g->p_bvy += ((g->p_by + g->p_bs / 2) - (g->p_rpy + g->p_ph / 2)) / 8;
    }

    if (g->p_bx < 0)        { g->p_rs++; pong_serve(g, 1); }
    if (g->p_bx > g->cw)    { g->p_ls++; pong_serve(g, 0); }
}

static void pong_draw(Game *g)
{
    gclear(g, fb_rgb(14, 24, 20));
    color_t net = fb_rgb(40, 70, 56);
    for (int y = 8; y < g->ch; y += 28) gfill(g, g->cw / 2 - 2, y, 4, 14, net);
    color_t w = fb_rgb(238, 245, 240);
    gfill(g, 14, g->p_lpy, g->p_pw, g->p_ph, w);
    gfill(g, g->cw - 14 - g->p_pw, g->p_rpy, g->p_pw, g->p_ph, w);
    gfill(g, g->p_bx, g->p_by, g->p_bs, g->p_bs, w);
}

static void pong_overlay(Game *g, int cx, int cy)
{
    char b[20];
    kutoa((uint32_t)g->p_ls, b);
    label(cx + g->cw / 2 - 60, cy + 8, b, fb_rgb(150, 230, 170), fb_rgb(14, 24, 20));
    kutoa((uint32_t)g->p_rs, b);
    label(cx + g->cw / 2 + 44, cy + 8, b, fb_rgb(230, 170, 150), fb_rgb(14, 24, 20));
    label(cx + 8, cy + g->ch - 22, "You", fb_rgb(150, 230, 170), fb_rgb(14, 24, 20));
    label(cx + g->cw - 36, cy + g->ch - 22, "CPU", fb_rgb(230, 170, 150), fb_rgb(14, 24, 20));
}

/* ===========================================================================
 * MARIO-STYLE PLATFORMER
 * ======================================================================== */

static char m_at(Game *g, int tx, int ty)
{
    if (tx < 0 || tx >= g->m_W || ty < 0 || ty >= g->m_H) return ' ';
    return g->m_map[ty * g->m_W + tx];
}
static int m_solid(char c) { return c == '#' || c == '?'; }

static int m_hits(Game *g, int x, int y)
{
    if (x < 0) return 1;
    int x0 = x / TS, x1 = (x + PW - 1) / TS;
    int y0 = y / TS, y1 = (y + PH - 1) / TS;
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++)
            if (m_solid(m_at(g, tx, ty))) return 1;
    return 0;
}

static void m_setrow(Game *g, int r, int c0, int c1, char ch)
{
    for (int c = c0; c <= c1; c++)
        if (c >= 0 && c < g->m_W && r >= 0 && r < g->m_H) g->m_map[r * g->m_W + c] = ch;
}

static void m_addenemy(Game *g, int col)
{
    if (g->m_ne >= 8) return;
    int i = g->m_ne++;
    g->e_x[i]  = col * TS;
    g->e_y[i]  = 12 * TS - ES;
    g->e_lo[i] = (col - 3) * TS; if (g->e_lo[i] < 0) g->e_lo[i] = 0;
    g->e_hi[i] = (col + 3) * TS;
    g->e_vx[i] = -1;
    g->e_on[i] = 1;
}

static void mario_reset(Game *g)
{
    g->over = 0; g->won = 0; g->score = 0; g->m_lives = 3;
    g->m_W = 60; g->m_H = 15;
    for (int i = 0; i < g->m_W * g->m_H; i++) g->m_map[i] = ' ';

    for (int c = 0; c < g->m_W; c++)
        for (int r = 12; r < 15; r++) g->m_map[r * g->m_W + c] = '#';
    int pits[] = { 17, 18, 33, 34, 35, -1 };
    for (int k = 0; pits[k] >= 0; k++)
        for (int r = 12; r < 15; r++) g->m_map[r * g->m_W + pits[k]] = ' ';

    m_setrow(g, 9, 8, 11, '#');  m_setrow(g, 8, 8, 11, 'o');
    m_setrow(g, 7, 22, 25, '#'); m_setrow(g, 6, 22, 25, 'o');
    m_setrow(g, 6, 38, 41, '#'); m_setrow(g, 5, 38, 41, 'o');
    g->m_map[10 * g->m_W + 5] = '?';  g->m_map[10 * g->m_W + 6] = '?';

    for (int r = 7; r < 12; r++) g->m_map[r * g->m_W + 57] = 'G';   /* flag pole */

    g->m_ne = 0;
    m_addenemy(g, 12); m_addenemy(g, 26); m_addenemy(g, 45);

    g->m_sx = 2 * TS; g->m_sy = 12 * TS - PH;
    g->m_px = g->m_sx; g->m_py = g->m_sy;
    g->m_vx = g->m_vy = 0; g->m_on = 0; g->m_cam = 0;
}

static void mario_respawn(Game *g)
{
    g->m_lives--;
    if (g->m_lives <= 0) { g->over = 1; if (g->score > g->best) g->best = g->score; return; }
    g->m_px = g->m_sx; g->m_py = g->m_sy; g->m_vx = g->m_vy = 0; g->m_on = 0;
}

static void mario_step(Game *g)
{
    if (g->over || g->won) { if (act_press()) mario_reset(g); return; }

    g->m_vx = 0;
    if (left_held())  g->m_vx -= 3;
    if (right_held()) g->m_vx += 3;
    if (act_press() && g->m_on) { g->m_vy = -12; g->m_on = 0; }

    g->m_vy += 1; if (g->m_vy > 14) g->m_vy = 14;

    int dir = g->m_vx > 0 ? 1 : -1;
    for (int s = 0; s < (g->m_vx < 0 ? -g->m_vx : g->m_vx); s++) {
        if (m_hits(g, g->m_px + dir, g->m_py)) break;
        g->m_px += dir;
    }
    dir = g->m_vy > 0 ? 1 : -1;
    for (int s = 0; s < (g->m_vy < 0 ? -g->m_vy : g->m_vy); s++) {
        if (m_hits(g, g->m_px, g->m_py + dir)) { if (dir > 0) g->m_on = 1; g->m_vy = 0; break; }
        g->m_py += dir;
    }
    g->m_on = m_hits(g, g->m_px, g->m_py + 1);

    /* coins / goal under the player's box */
    int x0 = g->m_px / TS, x1 = (g->m_px + PW - 1) / TS;
    int y0 = g->m_py / TS, y1 = (g->m_py + PH - 1) / TS;
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++) {
            char c = m_at(g, tx, ty);
            if (c == 'o') { g->m_map[ty * g->m_W + tx] = ' '; g->score += 100; }
            else if (c == 'G') { g->won = 1; g->score += 1000;
                                 if (g->score > g->best) g->best = g->score; }
        }

    for (int i = 0; i < g->m_ne; i++) {
        if (!g->e_on[i]) continue;
        g->e_x[i] += g->e_vx[i];
        if (g->e_x[i] < g->e_lo[i]) { g->e_x[i] = g->e_lo[i]; g->e_vx[i] = -g->e_vx[i]; }
        if (g->e_x[i] > g->e_hi[i]) { g->e_x[i] = g->e_hi[i]; g->e_vx[i] = -g->e_vx[i]; }

        if (g->m_px + PW > g->e_x[i] && g->m_px < g->e_x[i] + ES &&
            g->m_py + PH > g->e_y[i] && g->m_py < g->e_y[i] + ES) {
            if (g->m_vy > 0 && g->m_py + PH - g->m_vy <= g->e_y[i] + 8) {
                g->e_on[i] = 0; g->score += 200; g->m_vy = -8;      /* stomp */
            } else {
                mario_respawn(g);
                return;
            }
        }
    }

    if (g->m_py > g->m_H * TS + 80) mario_respawn(g);

    g->m_cam = g->m_px + PW / 2 - g->cw / 2;
    int cammax = g->m_W * TS - g->cw;
    if (cammax < 0) cammax = 0;
    if (g->m_cam < 0) g->m_cam = 0;
    if (g->m_cam > cammax) g->m_cam = cammax;
}

static void mario_draw(Game *g)
{
    int oy = g->ch - g->m_H * TS;          /* sit the level on the window floor */
    gclear(g, fb_rgb(108, 178, 240));

    int cam = g->m_cam;
    int tc0 = cam / TS, tc1 = (cam + g->cw) / TS + 1;
    for (int tx = tc0; tx <= tc1; tx++)
        for (int ty = 0; ty < g->m_H; ty++) {
            char c = m_at(g, tx, ty);
            if (c == ' ') continue;
            int sx = tx * TS - cam, sy = ty * TS + oy;
            if (c == '#') {
                gfill(g, sx, sy, TS, TS, fb_rgb(150, 92, 52));
                gfill(g, sx, sy, TS, 4, fb_rgb(98, 196, 84));
                gfill(g, sx, sy, 2, TS, fb_rgb(120, 72, 40));
            } else if (c == '?') {
                gfill(g, sx, sy, TS, TS, fb_rgb(232, 178, 48));
                gfill(g, sx + 2, sy + 2, TS - 4, TS - 4, fb_rgb(248, 206, 84));
                gfill(g, sx + TS / 2 - 2, sy + 6, 4, 8, fb_rgb(120, 80, 20));
                gfill(g, sx + TS / 2 - 2, sy + 16, 4, 3, fb_rgb(120, 80, 20));
            } else if (c == 'o') {
                gfill(g, sx + TS / 2 - 5, sy + TS / 2 - 7, 10, 14, fb_rgb(252, 214, 72));
                gfill(g, sx + TS / 2 - 2, sy + TS / 2 - 7, 4, 14, fb_rgb(255, 240, 160));
            } else if (c == 'G') {
                gfill(g, sx + TS / 2 - 1, sy, 3, TS, fb_rgb(220, 224, 230));
                if (ty == 7) gfill(g, sx + TS / 2 + 2, sy + 2, 12, 9, fb_rgb(220, 64, 64));
            }
        }

    for (int i = 0; i < g->m_ne; i++) {
        if (!g->e_on[i]) continue;
        int sx = g->e_x[i] - cam, sy = g->e_y[i] + oy;
        gfill(g, sx, sy, ES, ES, fb_rgb(140, 78, 44));
        gfill(g, sx, sy + ES - 5, ES, 5, fb_rgb(60, 40, 24));
        gfill(g, sx + 4, sy + 6, 4, 4, fb_rgb(255, 255, 255));
        gfill(g, sx + ES - 8, sy + 6, 4, 4, fb_rgb(255, 255, 255));
    }

    int sx = g->m_px - cam, sy = g->m_py + oy;
    gfill(g, sx, sy, PW, PH, fb_rgb(210, 60, 50));            /* shirt */
    gfill(g, sx, sy + PH / 2, PW, PH / 2, fb_rgb(60, 88, 200)); /* overalls */
    gfill(g, sx + 2, sy + 2, PW - 4, 6, fb_rgb(245, 206, 170)); /* face */
    gfill(g, sx, sy, PW, 4, fb_rgb(180, 40, 36));            /* cap */
}

static void mario_overlay(Game *g, int cx, int cy)
{
    char b[40];
    numlabel(b, "Score ", g->score);
    label(cx + 8, cy + 6, b, fb_rgb(255, 255, 255), fb_rgb(40, 60, 40));
    numlabel(b, "Lives ", g->m_lives);
    label(cx + g->cw - 90, cy + 6, b, fb_rgb(255, 230, 160), fb_rgb(40, 60, 40));
    if (g->won)
        center_msg(cx, cy, g->cw, g->ch, "YOU WIN!  -  SPACE", fb_rgb(255, 255, 160), fb_rgb(40, 120, 60));
    else if (g->over)
        center_msg(cx, cy, g->cw, g->ch, "GAME OVER  -  SPACE", fb_rgb(255, 220, 120), fb_rgb(120, 36, 44));
}

/* ===========================================================================
 * MAZE 3D  (raycaster)
 * ======================================================================== */

static const char *RC[16] = {
    "1111111111111111",
    "1000000000000001",
    "1011110011110101",
    "1000010000010001",
    "1011010110010111",
    "1000000100000001",
    "1111010111110101",
    "1000000000000001",
    "1011113011110101",
    "1000001000000001",
    "1011001011110111",
    "1000000000010001",
    "1010111110020101",
    "1010000000000001",
    "1011111111111101",
    "1111111111111111",
};

static int rc_cell(int cx, int cy)
{
    if (cx < 0 || cx >= 16 || cy < 0 || cy >= 16) return 1;
    char c = RC[cy][cx];
    return (c == '0') ? 0 : (c - '0');
}

static void raycast_reset(Game *g)
{
    g->r_px = 1 * 256 + 128;     /* cell (1,1) centre */
    g->r_py = 1 * 256 + 128;
    g->r_ang = 30;
    g->over = 0;
}

static void raycast_step(Game *g)
{
    if (left_held())  g->r_ang -= 4;
    if (right_held()) g->r_ang += 4;
    g->r_ang %= 360; if (g->r_ang < 0) g->r_ang += 360;

    int spd = 9;
    int dx = (cosd(g->r_ang) * spd) >> 10;
    int dy = (sind(g->r_ang) * spd) >> 10;
    int mv = up_held() ? 1 : (down_held() ? -1 : 0);
    if (mv) {
        int nx = g->r_px + dx * mv, ny = g->r_py + dy * mv;
        if (!rc_cell(nx >> 8, g->r_py >> 8)) g->r_px = nx;
        if (!rc_cell(g->r_px >> 8, ny >> 8)) g->r_py = ny;
    }
}

static void raycast_draw(Game *g)
{
    int W = g->cw, H = g->ch, half = H / 2;
    const int fov = 60, span = 2;
    color_t ceil = fb_rgb(28, 32, 50), floor = fb_rgb(58, 56, 52);

    for (int col = 0; col < W; col += span) {
        int rayang = g->r_ang - fov / 2 + (fov * col) / W;
        int ca = cosd(rayang), sa = sind(rayang);

        int x = g->r_px, y = g->r_py;
        int pcx = x >> 8, pcy = y >> 8;
        int dist = 0, side = 0, wallc = 1, hit = 0;
        const int stepfp = 24;
        for (int s = 0; s < 240; s++) {
            x += (ca * stepfp) >> 10;
            y += (sa * stepfp) >> 10;
            dist += stepfp;
            int cx = x >> 8, cy = y >> 8;
            if (cx != pcx) side = 0; else if (cy != pcy) side = 1;
            pcx = cx; pcy = cy;
            int v = rc_cell(cx, cy);
            if (v) { wallc = v; hit = 1; break; }
        }
        if (!hit) dist = 240 * stepfp;

        int corr = (dist * cosd(rayang - g->r_ang)) >> 10;
        if (corr < 16) corr = 16;
        int wallh = (H * 256) / corr;
        if (wallh > H) wallh = H;
        int top = half - wallh / 2;

        int s = 255 - corr / 6; if (s < 45) s = 45;
        if (side == 1) s = s * 3 / 4;
        color_t wcol;
        if      (wallc == 2) wcol = shade(70, 132, 190, s);
        else if (wallc == 3) wcol = shade(176, 150, 86, s);
        else                 wcol = shade(186, 74, 70, s);

        int w = span; if (col + w > W) w = W - col;
        gfill(g, col, 0, w, top, ceil);
        gfill(g, col, top, w, wallh, wcol);
        gfill(g, col, top + wallh, w, H - (top + wallh), floor);
    }
}

static void raycast_overlay(Game *g, int cx, int cy)
{
    (void)g;
    label(cx + 8, cy + 6, "Maze 3D  -  WASD / arrows", fb_rgb(220, 230, 245), fb_rgb(20, 24, 36));
}

/* ===========================================================================
 * TETRIS
 * ======================================================================== */

static const uint16_t TET[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },   /* I */
    { 0x6600, 0x6600, 0x6600, 0x6600 },   /* O */
    { 0x4E00, 0x4640, 0x0E40, 0x4C40 },   /* T */
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 },   /* S */
    { 0xC600, 0x2640, 0x0C60, 0x4C80 },   /* Z */
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 },   /* J */
    { 0x2E00, 0x4460, 0x0E80, 0xC440 },   /* L */
};

static color_t tet_color(int t)
{
    switch (t) {
    case 0: return fb_rgb(80, 200, 220);   /* I cyan   */
    case 1: return fb_rgb(228, 206, 70);   /* O yellow */
    case 2: return fb_rgb(170, 90, 200);   /* T purple */
    case 3: return fb_rgb(96, 200, 100);   /* S green  */
    case 4: return fb_rgb(220, 80, 80);    /* Z red    */
    case 5: return fb_rgb(80, 120, 220);   /* J blue   */
    default:return fb_rgb(228, 150, 64);   /* L orange */
    }
}

static int tet_valid(Game *g, int type, int rot, int x, int y)
{
    uint16_t m = TET[type][rot];
    for (int n = 0; n < 16; n++) {
        if (!(m & (0x8000 >> n))) continue;
        int bx = x + (n % 4), by = y + (n / 4);
        if (bx < 0 || bx >= COLS || by >= ROWS) return 0;
        if (by >= 0 && g->t_board[by * COLS + bx]) return 0;
    }
    return 1;
}

static void tet_spawn(Game *g)
{
    g->t_type = grange(0, 7);
    g->t_rot = 0; g->t_x = 3; g->t_y = 0; g->t_acc = 0;
    if (!tet_valid(g, g->t_type, g->t_rot, g->t_x, g->t_y)) {
        g->over = 1; if (g->score > g->best) g->best = g->score;
    }
}

static void tetris_reset(Game *g)
{
    for (int i = 0; i < ROWS * COLS; i++) g->t_board[i] = 0;
    g->over = 0; g->score = 0; g->t_lines = 0;
    tet_spawn(g);
}

static void tet_lock(Game *g)
{
    uint16_t m = TET[g->t_type][g->t_rot];
    for (int n = 0; n < 16; n++) {
        if (!(m & (0x8000 >> n))) continue;
        int bx = g->t_x + (n % 4), by = g->t_y + (n / 4);
        if (by >= 0 && by < ROWS && bx >= 0 && bx < COLS)
            g->t_board[by * COLS + bx] = (uint8_t)(g->t_type + 1);
    }
    int cleared = 0;
    for (int r = ROWS - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < COLS; c++) if (!g->t_board[r * COLS + c]) { full = 0; break; }
        if (full) {
            for (int rr = r; rr > 0; rr--)
                for (int c = 0; c < COLS; c++) g->t_board[rr * COLS + c] = g->t_board[(rr - 1) * COLS + c];
            for (int c = 0; c < COLS; c++) g->t_board[c] = 0;
            cleared++; r++;          /* re-check the row that shifted down */
        }
    }
    if (cleared) { g->t_lines += cleared; g->score += cleared * cleared * 100; }
    tet_spawn(g);
}

static void tetris_step(Game *g)
{
    if (g->over) { if (act_press()) tetris_reset(g); return; }

    if (left_press()  && tet_valid(g, g->t_type, g->t_rot, g->t_x - 1, g->t_y)) g->t_x--;
    if (right_press() && tet_valid(g, g->t_type, g->t_rot, g->t_x + 1, g->t_y)) g->t_x++;
    if (up_press()) {
        int nr = (g->t_rot + 1) & 3;
        if      (tet_valid(g, g->t_type, nr, g->t_x, g->t_y))     g->t_rot = nr;
        else if (tet_valid(g, g->t_type, nr, g->t_x - 1, g->t_y)) { g->t_rot = nr; g->t_x--; }
        else if (tet_valid(g, g->t_type, nr, g->t_x + 1, g->t_y)) { g->t_rot = nr; g->t_x++; }
    }
    if (space_press()) {                                   /* hard drop */
        while (tet_valid(g, g->t_type, g->t_rot, g->t_x, g->t_y + 1)) g->t_y++;
        tet_lock(g);
        return;
    }

    int level = g->t_lines / 10;
    int interval = 600 - level * 45; if (interval < 90) interval = 90;
    g->t_acc += 33;
    if (down_held()) g->t_acc += 33 * 6;                   /* soft drop */
    if (g->t_acc >= interval) {
        g->t_acc = 0;
        if (tet_valid(g, g->t_type, g->t_rot, g->t_x, g->t_y + 1)) g->t_y++;
        else tet_lock(g);
    }
}

static void tetris_draw(Game *g)
{
    gclear(g, fb_rgb(18, 20, 30));
    int cs = (g->cw - 20) / COLS;
    int ch2 = (g->ch - 30) / ROWS;
    if (ch2 < cs) cs = ch2;
    if (cs < 4) cs = 4;
    int bw = cs * COLS, bh = cs * ROWS;
    int ox = (g->cw - bw) / 2, oy = (g->ch - bh) / 2 + 6;

    gfill(g, ox - 3, oy - 3, bw + 6, bh + 6, fb_rgb(60, 66, 90));
    gfill(g, ox, oy, bw, bh, fb_rgb(10, 12, 18));

    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            uint8_t v = g->t_board[r * COLS + c];
            if (!v) continue;
            color_t col = tet_color(v - 1);
            gfill(g, ox + c * cs, oy + r * cs, cs - 1, cs - 1, col);
        }

    uint16_t m = TET[g->t_type][g->t_rot];
    color_t col = tet_color(g->t_type);
    for (int n = 0; n < 16; n++) {
        if (!(m & (0x8000 >> n))) continue;
        int bx = g->t_x + (n % 4), by = g->t_y + (n / 4);
        if (by < 0) continue;
        gfill(g, ox + bx * cs, oy + by * cs, cs - 1, cs - 1, col);
    }
}

static void tetris_overlay(Game *g, int cx, int cy)
{
    char b[40];
    numlabel(b, "Score ", g->score);
    label(cx + 8, cy + 6, b, fb_rgb(255, 255, 255), fb_rgb(18, 20, 30));
    numlabel(b, "Lines ", g->t_lines);
    label(cx + g->cw - 90, cy + 6, b, fb_rgb(180, 220, 255), fb_rgb(18, 20, 30));
    if (g->over)
        center_msg(cx, cy, g->cw, g->ch, "GAME OVER  -  SPACE", fb_rgb(255, 220, 120), fb_rgb(120, 36, 44));
}

/* ===========================================================================
 * Dispatch + lifecycle
 * ======================================================================== */

static void game_reset(Game *g)
{
    switch (g->kind) {
    case GAME_FLAPPY:  flappy_reset(g);  break;
    case GAME_PONG:    pong_reset(g);    break;
    case GAME_MARIO:   mario_reset(g);   break;
    case GAME_RAYCAST: raycast_reset(g); break;
    case GAME_TETRIS:  tetris_reset(g);  break;
    default: break;
    }
}

static void game_advance(Game *g)
{
    switch (g->kind) {
    case GAME_FLAPPY:  flappy_step(g);  break;
    case GAME_PONG:    pong_step(g);    break;
    case GAME_MARIO:   mario_step(g);   break;
    case GAME_RAYCAST: raycast_step(g); break;
    case GAME_TETRIS:  tetris_step(g);  break;
    default: break;
    }
}

static void game_compose(Game *g)
{
    switch (g->kind) {
    case GAME_FLAPPY:  flappy_draw(g);  break;
    case GAME_PONG:    pong_draw(g);    break;
    case GAME_MARIO:   mario_draw(g);   break;
    case GAME_RAYCAST: raycast_draw(g); break;
    case GAME_TETRIS:  tetris_draw(g);  break;
    default: gclear(g, fb_rgb(0, 0, 0)); break;
    }
}

static void game_overlay(Game *g, int cx, int cy)
{
    switch (g->kind) {
    case GAME_FLAPPY:  flappy_overlay(g, cx, cy);  break;
    case GAME_PONG:    pong_overlay(g, cx, cy);    break;
    case GAME_MARIO:   mario_overlay(g, cx, cy);   break;
    case GAME_RAYCAST: raycast_overlay(g, cx, cy); break;
    case GAME_TETRIS:  tetris_overlay(g, cx, cy);  break;
    default: break;
    }
}

static uint32_t game_step_ms(Game *g)
{
    switch (g->kind) {
    case GAME_FLAPPY:  return 28;
    case GAME_PONG:    return 16;
    case GAME_MARIO:   return 22;
    case GAME_RAYCAST: return 33;
    case GAME_TETRIS:  return 33;
    default: return 33;
    }
}

/* (re)allocate the off-screen buffer; reset the game the first time we know our
 * real size. Returns 1 if a buffer is available. */
static int ensure_buf(Game *g, int cw, int ch)
{
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;
    if (g->fbuf && g->cw == cw && g->ch == ch) return 1;
    if (g->fbuf) kfree(g->fbuf);
    g->fbuf = (color_t *)kmalloc((size_t)cw * ch * sizeof(color_t));
    g->cw = cw; g->ch = ch;
    if (!g->fbuf) return 0;
    if (!g->inited) { game_reset(g); g->inited = 1; }
    return 1;
}

/* Compose + present. Caller decides whether to bracket with mouse_hide/show:
 * game_paint() runs inside wm_paint() which already hides the cursor, while
 * game_tick() runs on its own and must hide/show the cursor itself. */
static void present_raw(Game *g, int cx, int cy)
{
    game_compose(g);
    fb_blit(cx, cy, g->cw, g->ch, g->fbuf);
    game_overlay(g, cx, cy);
}

static void present(Game *g, int cx, int cy)
{
    mouse_hide();
    present_raw(g, cx, cy);
    mouse_show();
}

const char *game_title(int kind)
{
    switch (kind) {
    case GAME_FLAPPY:  return "Flappy";
    case GAME_PONG:    return "Pong";
    case GAME_MARIO:   return "Mario";
    case GAME_RAYCAST: return "Maze 3D";
    case GAME_TETRIS:  return "Tetris";
    default: return "Game";
    }
}

void *game_new(int kind)
{
    module_init();
    Game *g = (Game *)kmalloc(sizeof(Game));
    if (!g) return NULL;
    kmemset(g, 0, sizeof(Game));
    g->kind = kind;
    return g;
}

void game_free(void *state)
{
    Game *g = (Game *)state;
    if (!g) return;
    if (g->fbuf) kfree(g->fbuf);
    kfree(g);
}

void game_paint(Window *w, int cx, int cy, int cw, int ch)
{
    Game *g = (Game *)w->state;
    if (!g) return;
    if (!ensure_buf(g, cw, ch)) { fb_fill_rect(cx, cy, cw, ch, fb_rgb(0, 0, 0)); return; }
    present_raw(g, cx, cy);                 /* inside wm_paint's mouse bracket */
}

void game_tick(Window *w, int cx, int cy, int cw, int ch)
{
    Game *g = (Game *)w->state;
    if (!g) return;
    if (!ensure_buf(g, cw, ch)) return;

    uint32_t now = clock_ms();
    uint32_t step = game_step_ms(g);
    if (g->last_ms == 0) g->last_ms = now;
    if (now - g->last_ms < step) return;
    g->last_ms += step;
    if (now - g->last_ms > step * 4) g->last_ms = now;   /* don't spiral if behind */

    game_advance(g);
    present(g, cx, cy);
}

void game_key(Window *w, char c)
{
    Game *g = (Game *)w->state;
    if (!g) return;
    if (c == 'r' || c == 'R') { game_reset(g); }
}

void game_resize(Window *w, int cw, int ch)
{
    Game *g = (Game *)w->state;
    if (!g) return;
    ensure_buf(g, cw, ch);
}
