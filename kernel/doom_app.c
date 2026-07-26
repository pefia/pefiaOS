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

/* Embedded by boot/doom_wad.asm -- the raw bytes of doom1.wad live right
 * in the kernel binary between these two symbols. */
extern const unsigned char doom1_wad_start[];
extern const unsigned char doom1_wad_end[];

/* --- tiny string helpers, since we can't pull in a real libc here --- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int str_has(const char *s, char ch)
{
    for (; *s; s++)
        if (*s == ch) return 1;
    return 0;
}

/* substring test -- brute force is plenty for the handful of filenames
 * DOOM ever asks us to open. */
static int str_contains(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void str_copy_bounded(char *dst, const char *src, int cap)
{
    int i = 0;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ---------------------------------------------------------------------
 * A fake filesystem, because PureDOOM insists on fopen-style access
 * for its WAD and its save/config files. Everything lives in a small
 * fixed table -- one slot is a read-only view over the embedded WAD,
 * the rest are heap-backed scratch buffers big enough for a savegame.
 * ------------------------------------------------------------------ */

#define MAX_OPEN_FILES  10
#define SCRATCH_BYTES   (256 * 1024)

typedef struct {
    int   in_use;
    char  name[40];
    const unsigned char *rd;   /* points into the embedded WAD, or NULL */
    unsigned char       *wr;
    int   size;
    int   cap;
    int   pos;
} VFile;

static VFile g_vfiles[MAX_OPEN_FILES];

static VFile *vfile_find(const char *name)
{
    int i;
    for (i = 0; i < MAX_OPEN_FILES; i++)
        if (g_vfiles[i].in_use && str_eq(g_vfiles[i].name, name))
            return &g_vfiles[i];
    return NULL;
}

static VFile *vfile_claim_slot(const char *name)
{
    int i;
    VFile *existing = vfile_find(name);
    if (existing) return existing;

    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_vfiles[i].in_use) continue;
        VFile *f = &g_vfiles[i];
        f->in_use = 1;
        f->rd = NULL;
        f->wr = NULL;
        f->size = f->cap = f->pos = 0;
        str_copy_bounded(f->name, name, (int)sizeof(f->name));
        return f;
    }
    return NULL;   /* table's full -- shouldn't happen with only 10 slots in play */
}

static void *vfs_open(const char *filename, const char *mode)
{
    int wants_write = str_has(mode, 'w') || str_has(mode, 'a') || str_has(mode, '+');

    if (!wants_write) {
        if (str_contains(filename, "doom1.wad")) {
            VFile *f = vfile_claim_slot(filename);
            if (!f) return NULL;
            f->rd   = doom1_wad_start;
            f->size = (int)(doom1_wad_end - doom1_wad_start);
            f->wr   = NULL;
            f->pos  = 0;
            return f;
        }
        /* not the WAD -- see if it's a scratch file we wrote earlier */
        VFile *f = vfile_find(filename);
        if (f && (f->rd || f->wr)) { f->pos = 0; return f; }
        return NULL;
    }

    /* write mode: (re)use a scratch slot, allocating its backing buffer lazily */
    VFile *f = vfile_claim_slot(filename);
    if (!f) return NULL;
    if (!f->wr) {
        f->wr = (unsigned char *)kmalloc(SCRATCH_BYTES);
        if (!f->wr) { f->in_use = 0; return NULL; }
        f->cap = SCRATCH_BYTES;
    }
    f->rd = NULL;
    f->size = 0;
    f->pos  = 0;
    return f;
}

static void vfs_close(void *handle)
{
    VFile *f = (VFile *)handle;
    if (!f) return;
    if (f->rd) f->in_use = 0;   /* WAD view was only ever borrowed, hand the slot back */
    else       f->pos = 0;     /* scratch file: keep the data around for next open()  */
}

static int vfs_read(void *handle, void *buf, int count)
{
    VFile *f = (VFile *)handle;
    if (!f) return 0;
    const unsigned char *src = f->rd ? f->rd : f->wr;
    if (!src) return 0;

    int avail = f->size - f->pos;
    if (count > avail) count = avail;
    if (count <= 0) return 0;

    for (int i = 0; i < count; i++)
        ((unsigned char *)buf)[i] = src[f->pos + i];
    f->pos += count;
    return count;
}

static int vfs_write(void *handle, const void *buf, int count)
{
    VFile *f = (VFile *)handle;
    if (!f || !f->wr) return 0;   /* can't write into the read-only WAD view */

    if (f->pos + count > f->cap) count = f->cap - f->pos;
    if (count <= 0) return 0;

    for (int i = 0; i < count; i++)
        f->wr[f->pos + i] = ((const unsigned char *)buf)[i];
    f->pos += count;
    if (f->pos > f->size) f->size = f->pos;
    return count;
}

static int vfs_seek(void *handle, int offset, doom_seek_t origin)
{
    VFile *f = (VFile *)handle;
    if (!f) return -1;

    int base = (origin == DOOM_SEEK_CUR) ? f->pos
             : (origin == DOOM_SEEK_END) ? f->size : 0;
    int target = base + offset;
    int limit  = f->wr ? f->cap : f->size;

    if (target < 0) target = 0;
    if (target > limit) target = limit;
    f->pos = target;
    return 0;
}

static int vfs_tell(void *handle)
{
    VFile *f = (VFile *)handle;
    return f ? f->pos : -1;
}

static int vfs_eof(void *handle)
{
    VFile *f = (VFile *)handle;
    return (f && f->pos >= f->size) ? 1 : 0;
}

/* --- the rest of the callback table: print/alloc/time/exit/env --- */

static void  doom_stdout(const char *str) { (void)str; /* nowhere to send this, drop it */ }
static void *doom_heap_alloc(int size)    { return kmalloc((size_t)size); }
static void  doom_heap_free(void *ptr)    { kfree(ptr); }

static void doom_clock(int *sec, int *usec)
{
    uint32_t ms = clock_ms();
    *sec  = (int)(ms / 1000);
    *usec = (int)((ms % 1000) * 1000);
}

/* DOOM's I_Error() funnels here. Rather than let it take the whole
 * kernel down, we bail out with a longjmp back to whoever called
 * doom_update() this tick and just render a "it died" placeholder. */
static void *g_recover_point[5];
static int   g_engine_dead;

static void doom_bail(int code)
{
    (void)code;
    g_engine_dead = 1;
    __builtin_longjmp(g_recover_point, 1);
}

static char *doom_env(const char *var)
{
    if (str_eq(var, "HOME") || str_eq(var, "DOOMWADDIR")) return ".";
    return NULL;
}

/* ---------------------------------------------------------------------
 * Keyboard mapping: pefiaOS gives us raw PS/2 scancodes, DOOM wants its
 * own key constants (which for printable keys are just ASCII). This
 * table only needs to cover the keys the make/set-2 scancode set can
 * actually produce for a US layout -- nobody's going to be typing
 * accented characters into a DOOM savegame name.
 * ------------------------------------------------------------------ */

static const char scancode_ascii[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',
    [0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',
    [0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',
    [0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',
    [0x33]=',',[0x34]='.',[0x35]='/',
};

static int scancode_to_doom_key(int code, int extended)
{
    if (extended) {
        switch (code) {
        case 0x4B: return DOOM_KEY_LEFT_ARROW;
        case 0x4D: return DOOM_KEY_RIGHT_ARROW;
        case 0x48: return DOOM_KEY_UP_ARROW;
        case 0x50: return DOOM_KEY_DOWN_ARROW;
        case 0x1D: return DOOM_KEY_CTRL;
        case 0x38: return DOOM_KEY_ALT;
        case 0x1C: return DOOM_KEY_ENTER;   /* keypad Enter sends this as an extended code */
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
    case 0x2A: case 0x36: return DOOM_KEY_SHIFT;   /* left and right shift, we don't care which */
    case 0x38: return DOOM_KEY_ALT;
    }

    char mapped = scancode_ascii[code & 0x7F];
    return mapped ? (int)(unsigned char)mapped : -1;
}

/* ---------------------------------------------------------------------
 * Window app: DoomState is the per-window instance pefiaOS's WM hands
 * back to us on every paint/tick/resize call.
 * ------------------------------------------------------------------ */

typedef struct {
    color_t  *pixels;      /* off-screen buffer, sized to the window's client area */
    int       bw, bh;
    int       started;
    int       crashed;     /* did we recover from an I_Error? */
    uint32_t  last_tick_ms;
} DoomState;

static char *g_doom_argv[] = { "doom", NULL };

/* (Re)allocate the composite buffer if the window changed size. Clamped
 * to a sane minimum so a window dragged down to nothing doesn't leave
 * us multiplying by zero somewhere downstream. */
static int doom_state_resize_buffer(DoomState *s, int w, int h)
{
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (s->pixels && s->bw == w && s->bh == h) return 1;

    if (s->pixels) kfree(s->pixels);
    s->pixels = (color_t *)kmalloc((size_t)w * h * sizeof(color_t));
    s->bw = w;
    s->bh = h;
    return s->pixels != NULL;
}

static void doom_register_callbacks_and_boot(void)
{
    doom_set_print(doom_stdout);
    doom_set_malloc(doom_heap_alloc, doom_heap_free);
    doom_set_file_io(vfs_open, vfs_close, vfs_read, vfs_write, vfs_seek, vfs_tell, vfs_eof);
    doom_set_gettime(doom_clock);
    doom_set_exit(doom_bail);
    doom_set_getenv(doom_env);
    doom_set_resolution(DOOM_W, DOOM_H);
    doom_init(1, g_doom_argv, DOOM_FLAG_MENU_DARKEN_BG);
}

/* Nearest-neighbour upscale of DOOM's fixed 320x200 RGBA framebuffer
 * into the window buffer, centered and scaled to the largest integer
 * factor that still fits. Nothing fancy -- DOOM's software renderer
 * already did the hard work, we're just blowing up pixels. */
static void doom_compose_frame(DoomState *s)
{
    int i;
    for (i = 0; i < s->bw * s->bh; i++) s->pixels[i] = 0;

    const unsigned char *fb = doom_get_framebuffer(4);
    if (!fb) return;

    int scale = s->bw / DOOM_W;
    int scale_y = s->bh / DOOM_H;
    if (scale_y < scale) scale = scale_y;
    if (scale < 1) scale = 1;

    int drawn_w = DOOM_W * scale, drawn_h = DOOM_H * scale;
    int off_x = (s->bw - drawn_w) / 2, off_y = (s->bh - drawn_h) / 2;

    int y, x;
    for (y = 0; y < DOOM_H; y++) {
        for (x = 0; x < DOOM_W; x++) {
            const unsigned char *px = fb + (y * DOOM_W + x) * 4;
            color_t c = fb_rgb(px[0], px[1], px[2]);
            int bx = off_x + x * scale, by = off_y + y * scale;
            int yy, xx;
            for (yy = 0; yy < scale; yy++) {
                color_t *row = s->pixels + (long)(by + yy) * s->bw + bx;
                for (xx = 0; xx < scale; xx++) row[xx] = c;
            }
        }
    }
}

/* Draws whatever's appropriate for the current state: a loading screen
 * before doom_init() has run, an error box if it died, or the real
 * frame otherwise. */
static void doom_present(DoomState *s, int cx, int cy)
{
    if (s->crashed) {
        fb_fill_rect(cx, cy, s->bw, s->bh, fb_rgb(0, 0, 0));
        gfx_text(cx + 12, cy + 12, "DOOM stopped (I_Error).", fb_rgb(255, 90, 90), fb_rgb(0, 0, 0));
        return;
    }
    if (!s->started) {
        fb_fill_rect(cx, cy, s->bw, s->bh, fb_rgb(0, 0, 0));
        gfx_text(cx + 12, cy + 12, "Loading DOOM...", fb_rgb(220, 220, 220), fb_rgb(0, 0, 0));
        return;
    }
    doom_compose_frame(s);
    fb_blit(cx, cy, s->bw, s->bh, s->pixels);
}

void *doom_app_new(void)
{
    DoomState *s = (DoomState *)kmalloc(sizeof(DoomState));
    if (!s) return NULL;
    s->pixels = NULL;
    s->bw = s->bh = 0;
    s->started = 0;
    s->crashed = 0;
    s->last_tick_ms = 0;
    return s;
}

void doom_app_free(void *state)
{
    DoomState *s = (DoomState *)state;
    if (!s) return;
    if (s->pixels) kfree(s->pixels);
    kfree(s);
    /* Note: PureDOOM's own zone allocator and lump cache are global
     * state inside the engine -- closing this window doesn't reclaim
     * them. Fine for now since we only ever have one DOOM window. */
}

void doom_app_paint(Window *w, int cx, int cy, int cw, int ch)
{
    DoomState *s = (DoomState *)w->state;
    if (!s) return;
    if (!doom_state_resize_buffer(s, cw, ch)) {
        fb_fill_rect(cx, cy, cw, ch, fb_rgb(0, 0, 0));
        return;
    }
    doom_present(s, cx, cy);   /* called from inside wm_paint's mouse-cursor bracket */
}

void doom_app_tick(Window *w, int cx, int cy, int cw, int ch)
{
    DoomState *s = (DoomState *)w->state;
    if (!s) return;
    if (!doom_state_resize_buffer(s, cw, ch)) return;
    if (s->crashed) return;

    /* Cap DOOM at roughly 50Hz -- it doesn't need to run any faster
     * than the window manager calls us, and this keeps things from
     * outrunning input polling. */
    uint32_t now = clock_ms();
    if (s->last_tick_ms == 0) s->last_tick_ms = now;
    if (now - s->last_tick_ms < 20) return;
    s->last_tick_ms = now;

    if (__builtin_setjmp(g_recover_point) == 0) {
        if (!s->started) {
            doom_register_callbacks_and_boot();
            s->started = 1;
        }

        int code, pressed, extended;
        while (input_next_event(&code, &pressed, &extended)) {
            int key = scancode_to_doom_key(code, extended);
            if (key < 0) continue;
            if (pressed) doom_key_down(key);
            else         doom_key_up(key);
        }

        doom_update();

        mouse_hide();
        doom_present(s, cx, cy);
        mouse_show();
    } else {
        /* Landed here via longjmp from doom_bail() -- the engine hit
         * I_Error mid-update. w->state is still valid, just re-fetch
         * it since s may have been clobbered by the jump. */
        s = (DoomState *)w->state;
        s->crashed = 1;
        mouse_hide();
        doom_present(s, cx, cy);
        mouse_show();
    }
}

void doom_app_resize(Window *w, int cw, int ch)
{
    DoomState *s = (DoomState *)w->state;
    if (!s) return;
    doom_state_resize_buffer(s, cw, ch);
}
