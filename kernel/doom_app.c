/* kernel/doom_app.c
 * -----------------------------------------------------------------------------
 * pefiaOS platform layer for PureDOOM, wrapped as a window-manager app.
 *
 * PureDOOM is engine-only and dependency-free: it asks us for everything it
 * needs through callbacks. We give it:
 *   - memory      -> kmalloc / kfree
 *   - time        -> clock_ms()
 *   - file I/O    -> a tiny RAM filesystem. Reads of "*doom1.wad" are served
 *                    straight from the IWAD bytes embedded in the kernel image
 *                    (boot/doom_wad.asm); config/savegames live in RAM scratch
 *                    files so the engine doesn't choke writing them.
 *   - print/exit/getenv -> no-op / longjmp-out / minimal.
 *
 * Each frame we feed DOOM the queued keyboard events, run doom_update(), then
 * scale its 320x200 RGBA framebuffer into the window. If DOOM ever calls
 * I_Error (doom_exit), we longjmp back out and show a failure box instead of
 * taking the whole OS down.
 * -----------------------------------------------------------------------------
 */
#include "doom_app.h"
#include "wm.h"
#include "framebuffer.h"
#include "console.h"
#include "input.h"
#include "mouse.h"
#include "heap.h"
#include "clock.h"
#include "PureDOOM.h"

#include <stdint.h>
#include <stddef.h>

#define DOOM_W 320
#define DOOM_H 200

/* WAD bytes embedded by boot/doom_wad.asm */
extern const unsigned char doom1_wad_start[];
extern const unsigned char doom1_wad_end[];

/* ===========================================================================
 * Small string helpers (freestanding)
 * ======================================================================== */

static int s_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int s_has_char(const char *s, char c)
{
    for (; *s; s++) if (*s == c) return 1;
    return 0;
}
static int s_contains(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}
static void s_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ===========================================================================
 * RAM filesystem
 * ======================================================================== */

#define NFILE       10
#define SCRATCH_CAP (256 * 1024)   /* enough for a DOOM savegame */

typedef struct {
    int            used;
    char           name[40];
    const unsigned char *ro;   /* read-only source (the WAD), or NULL */
    unsigned char *rw;         /* writable scratch buffer, or NULL    */
    int            size;
    int            cap;
    int            pos;
} DFile;

static DFile g_files[NFILE];

static DFile *file_named(const char *name)
{
    for (int i = 0; i < NFILE; i++)
        if (g_files[i].used && s_eq(g_files[i].name, name)) return &g_files[i];
    return NULL;
}

static DFile *file_alloc(const char *name)
{
    DFile *f = file_named(name);
    if (f) return f;
    for (int i = 0; i < NFILE; i++)
        if (!g_files[i].used) {
            f = &g_files[i];
            f->used = 1; f->ro = NULL; f->rw = NULL;
            f->size = 0; f->cap = 0; f->pos = 0;
            s_copy(f->name, name, (int)sizeof(f->name));
            return f;
        }
    return NULL;
}

static void *d_open(const char *filename, const char *mode)
{
    int write = s_has_char(mode, 'w') || s_has_char(mode, 'a') || s_has_char(mode, '+');

    if (!write) {
        if (s_contains(filename, "doom1.wad")) {
            DFile *f = file_alloc(filename);
            if (!f) return NULL;
            f->ro   = doom1_wad_start;
            f->size = (int)(doom1_wad_end - doom1_wad_start);
            f->rw   = NULL;
            f->pos  = 0;
            return f;
        }
        DFile *f = file_named(filename);              /* read back a scratch file */
        if (f && (f->ro || f->rw)) { f->pos = 0; return f; }
        return NULL;                                  /* unknown file: not found  */
    }

    DFile *f = file_alloc(filename);
    if (!f) return NULL;
    if (!f->rw) {
        f->rw = (unsigned char *)kmalloc(SCRATCH_CAP);
        if (!f->rw) { f->used = 0; return NULL; }
        f->cap = SCRATCH_CAP;
    }
    f->ro = NULL; f->size = 0; f->pos = 0;
    return f;
}

static void d_close(void *handle)
{
    DFile *f = (DFile *)handle;
    if (!f) return;
    if (f->ro) f->used = 0;       /* WAD view: release the slot for reuse  */
    else f->pos = 0;             /* scratch: keep it so it can be re-read  */
}

static int d_read(void *handle, void *buf, int count)
{
    DFile *f = (DFile *)handle;
    if (!f) return 0;
    const unsigned char *src = f->ro ? f->ro : f->rw;
    if (!src) return 0;
    int avail = f->size - f->pos;
    if (count > avail) count = avail;
    if (count <= 0) return 0;
    for (int i = 0; i < count; i++) ((unsigned char *)buf)[i] = src[f->pos + i];
    f->pos += count;
    return count;
}

static int d_write(void *handle, const void *buf, int count)
{
    DFile *f = (DFile *)handle;
    if (!f || !f->rw) return 0;
    if (f->pos + count > f->cap) count = f->cap - f->pos;
    if (count <= 0) return 0;
    for (int i = 0; i < count; i++) f->rw[f->pos + i] = ((const unsigned char *)buf)[i];
    f->pos += count;
    if (f->pos > f->size) f->size = f->pos;
    return count;
}

static int d_seek(void *handle, int offset, doom_seek_t origin)
{
    DFile *f = (DFile *)handle;
    if (!f) return -1;
    int base = (origin == DOOM_SEEK_CUR) ? f->pos
             : (origin == DOOM_SEEK_END) ? f->size : 0;
    int np = base + offset;
    int limit = f->rw ? f->cap : f->size;
    if (np < 0) np = 0;
    if (np > limit) np = limit;
    f->pos = np;
    return 0;
}

static int d_tell(void *handle) { DFile *f = (DFile *)handle; return f ? f->pos : -1; }
static int d_eof (void *handle) { DFile *f = (DFile *)handle; return (f && f->pos >= f->size) ? 1 : 0; }

/* ===========================================================================
 * Other callbacks
 * ======================================================================== */

static void  d_print(const char *str) { (void)str; }
static void *d_malloc(int size)        { return kmalloc((size_t)size); }
static void  d_free(void *ptr)         { kfree(ptr); }

static void d_gettime(int *sec, int *usec)
{
    uint32_t ms = clock_ms();
    *sec  = (int)(ms / 1000);
    *usec = (int)((ms % 1000) * 1000);
}

static void *g_jmp[5];
static int   g_failed;
static void  d_exit(int code) { (void)code; g_failed = 1; __builtin_longjmp(g_jmp, 1); }

static char *d_getenv(const char *var)
{
    if (s_eq(var, "HOME") || s_eq(var, "DOOMWADDIR")) return ".";
    return NULL;
}

/* ===========================================================================
 * Keyboard: pefiaOS scancode -> DOOM key
 * ======================================================================== */

static const char ascii_map[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',
    [0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',
    [0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',
    [0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',
    [0x33]=',',[0x34]='.',[0x35]='/',
};

static int sc_to_doom(int code, int ext)
{
    if (ext) {
        switch (code) {
        case 0x4B: return DOOM_KEY_LEFT_ARROW;
        case 0x4D: return DOOM_KEY_RIGHT_ARROW;
        case 0x48: return DOOM_KEY_UP_ARROW;
        case 0x50: return DOOM_KEY_DOWN_ARROW;
        case 0x1D: return DOOM_KEY_CTRL;
        case 0x38: return DOOM_KEY_ALT;
        case 0x1C: return DOOM_KEY_ENTER;     /* keypad Enter */
        }
        return -1;
    }
    switch (code) {
    case 0x01: return DOOM_KEY_ESCAPE;
    case 0x1C: return DOOM_KEY_ENTER;
    case 0x0F: return DOOM_KEY_TAB;
    case 0x39: return DOOM_KEY_SPACE;
    case 0x0E: return DOOM_KEY_BACKSPACE;
    case 0x1D: return DOOM_KEY_CTRL;
    case 0x2A: case 0x36: return DOOM_KEY_SHIFT;
    case 0x38: return DOOM_KEY_ALT;
    }
    char c = ascii_map[code & 0x7F];
    return c ? (int)(unsigned char)c : -1;
}

/* ===========================================================================
 * The window app
 * ======================================================================== */

typedef struct {
    color_t  *buf;
    int       cw, ch;
    int       inited;
    int       failed;
    uint32_t  last_ms;
} DoomState;

static char *g_argv[] = { "doom", NULL };

static int ensure_buf(DoomState *s, int cw, int ch)
{
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;
    if (s->buf && s->cw == cw && s->ch == ch) return 1;
    if (s->buf) kfree(s->buf);
    s->buf = (color_t *)kmalloc((size_t)cw * ch * sizeof(color_t));
    s->cw = cw; s->ch = ch;
    return s->buf != NULL;
}

static void doom_setup(void)
{
    doom_set_print(d_print);
    doom_set_malloc(d_malloc, d_free);
    doom_set_file_io(d_open, d_close, d_read, d_write, d_seek, d_tell, d_eof);
    doom_set_gettime(d_gettime);
    doom_set_exit(d_exit);
    doom_set_getenv(d_getenv);
    doom_set_resolution(DOOM_W, DOOM_H);
    doom_init(1, g_argv, DOOM_FLAG_MENU_DARKEN_BG);
}

/* Scale DOOM's 320x200 RGBA framebuffer into the window's off-screen buffer. */
static void compose(DoomState *s)
{
    for (int i = 0; i < s->cw * s->ch; i++) s->buf[i] = 0;   /* black */

    const unsigned char *fb = doom_get_framebuffer(4);
    if (!fb) return;

    int scale = s->cw / DOOM_W;
    int sy    = s->ch / DOOM_H;
    if (sy < scale) scale = sy;
    if (scale < 1) scale = 1;

    int dw = DOOM_W * scale, dh = DOOM_H * scale;
    int ox = (s->cw - dw) / 2, oy = (s->ch - dh) / 2;

    for (int y = 0; y < DOOM_H; y++) {
        for (int x = 0; x < DOOM_W; x++) {
            const unsigned char *p = fb + (y * DOOM_W + x) * 4;
            color_t c = fb_rgb(p[0], p[1], p[2]);
            int bx = ox + x * scale, by = oy + y * scale;
            for (int yy = 0; yy < scale; yy++) {
                color_t *row = s->buf + (long)(by + yy) * s->cw + bx;
                for (int xx = 0; xx < scale; xx++) row[xx] = c;
            }
        }
    }
}

static void present_raw(DoomState *s, int cx, int cy)
{
    if (s->failed) {
        fb_fill_rect(cx, cy, s->cw, s->ch, fb_rgb(0, 0, 0));
        gfx_text(cx + 12, cy + 12, "DOOM stopped (I_Error).", fb_rgb(255, 90, 90), fb_rgb(0, 0, 0));
        return;
    }
    if (!s->inited) {
        fb_fill_rect(cx, cy, s->cw, s->ch, fb_rgb(0, 0, 0));
        gfx_text(cx + 12, cy + 12, "Loading DOOM...", fb_rgb(220, 220, 220), fb_rgb(0, 0, 0));
        return;
    }
    compose(s);
    fb_blit(cx, cy, s->cw, s->ch, s->buf);
}

void *doom_app_new(void)
{
    DoomState *s = (DoomState *)kmalloc(sizeof(DoomState));
    if (!s) return NULL;
    s->buf = NULL; s->cw = s->ch = 0;
    s->inited = 0; s->failed = 0; s->last_ms = 0;
    return s;
}

void doom_app_free(void *state)
{
    DoomState *s = (DoomState *)state;
    if (!s) return;
    if (s->buf) kfree(s->buf);
    kfree(s);
    /* PureDOOM's own allocations (zone, lumps) are global and not reclaimed. */
}

void doom_app_paint(Window *w, int cx, int cy, int cw, int ch)
{
    DoomState *s = (DoomState *)w->state;
    if (!s) return;
    if (!ensure_buf(s, cw, ch)) { fb_fill_rect(cx, cy, cw, ch, fb_rgb(0, 0, 0)); return; }
    present_raw(s, cx, cy);          /* inside wm_paint's mouse bracket */
}

void doom_app_tick(Window *w, int cx, int cy, int cw, int ch)
{
    DoomState *s = (DoomState *)w->state;
    if (!s) return;
    if (!ensure_buf(s, cw, ch)) return;
    if (s->failed) return;

    uint32_t now = clock_ms();
    if (s->last_ms == 0) s->last_ms = now;
    if (now - s->last_ms < 20) return;       /* ~50 Hz: input + frame pacing */
    s->last_ms = now;

    if (__builtin_setjmp(g_jmp) == 0) {
        if (!s->inited) { doom_setup(); s->inited = 1; }

        int code, pressed, ext;
        while (input_next_event(&code, &pressed, &ext)) {
            int k = sc_to_doom(code, ext);
            if (k >= 0) { if (pressed) doom_key_down(k); else doom_key_up(k); }
        }

        doom_update();

        mouse_hide();
        present_raw(s, cx, cy);
        mouse_show();
    } else {
        /* I_Error longjmp'd back to us. */
        s = (DoomState *)w->state;
        s->failed = 1;
        mouse_hide();
        present_raw(s, cx, cy);
        mouse_show();
    }
}

void doom_app_resize(Window *w, int cw, int ch)
{
    DoomState *s = (DoomState *)w->state;
    if (!s) return;
    ensure_buf(s, cw, ch);
}
