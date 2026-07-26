#include "framebuffer.h"
#include "multiboot.h"
#include "util.h"
#include "heap.h"

static uint8_t *fb_base;
static uint8_t *fb_hw;
static uint8_t *fb_back;
static int back_active;
static int fb_w, fb_h;
static int fb_pitch;
static int fb_bpp_val, fb_bytes_per_px;

/* Where each color channel lives within a pixel, straight from Multiboot's
 * color_info. */
static int r_shift, r_bits, g_shift, g_bits, b_shift, b_bits;

/* Dirty rectangle: the union of everything drawn since the last flip, so
 * fb_end_offscreen() only copies the pixels that actually changed instead
 * of the whole surface. x1/y1 are exclusive; empty when x0 >= x1. */
static int dmg_x0, dmg_y0, dmg_x1, dmg_y1;

static void dmg_reset(void)
{
    dmg_x0 = fb_w; dmg_y0 = fb_h;
    dmg_x1 = 0;    dmg_y1 = 0;
}

/* Grow the dirty rect to cover [x0,x1)x[y0,y1), clamped to the surface so
 * a partly-offscreen draw can never make the flip touch out-of-bounds
 * memory. Only meaningful while composing offscreen - direct drawing hits
 * the hardware immediately and must not smear stale back-buffer pixels
 * over it on the next flip. */
static void dmg_add(int x0, int y0, int x1, int y1)
{
    if (!back_active) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    if (x0 < dmg_x0) dmg_x0 = x0;
    if (y0 < dmg_y0) dmg_y0 = y0;
    if (x1 > dmg_x1) dmg_x1 = x1;
    if (y1 > dmg_y1) dmg_y1 = y1;
}

/* For callers that wrote pixels behind the driver's back: force the next
 * flip to copy the entire surface. */
void fb_damage_all(void)
{
    dmg_x0 = 0;    dmg_y0 = 0;
    dmg_x1 = fb_w; dmg_y1 = fb_h;
}

int fb_init(uint32_t magic, uint32_t info_addr)
{
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        return -1;

    struct multiboot_info *mb = (struct multiboot_info *)info_addr;
    if (!(mb->flags & MULTIBOOT_INFO_FRAMEBUFFER))
        return -1;
    if (mb->framebuffer_type != 1)   /* 1 = direct RGB; palettized modes not handled */
        return -1;

    fb_base    = (uint8_t *)(uint32_t)mb->framebuffer_addr;
    fb_hw      = fb_base;
    fb_w       = (int)mb->framebuffer_width;
    fb_h       = (int)mb->framebuffer_height;
    fb_pitch   = (int)mb->framebuffer_pitch;
    fb_bpp_val = mb->framebuffer_bpp;
    fb_bytes_per_px = fb_bpp_val / 8;

    r_shift = mb->color_info[0]; r_bits = mb->color_info[1];
    g_shift = mb->color_info[2]; g_bits = mb->color_info[3];
    b_shift = mb->color_info[4]; b_bits = mb->color_info[5];

    /* GRUB's emulated VBE modes routinely report bogus channel sizes (red
     * size of 0, that sort of thing). If any channel looks empty, fall back
     * to the standard layout for the given depth rather than trust it:
     * 32/24bpp is XRGB8888, 16bpp is RGB565. */
    if (!(r_bits && g_bits && b_bits)) {
        if (fb_bpp_val >= 24) {
            r_shift = 16; r_bits = 8; g_shift = 8; g_bits = 8; b_shift = 0; b_bits = 8;
        } else {
            r_shift = 11; r_bits = 5; g_shift = 5; g_bits = 6; b_shift = 0; b_bits = 5;
        }
    }

    /* The back buffer starts out as whatever kmalloc hands us, so the first
     * flip after a mode set must push the whole surface. */
    fb_damage_all();
    return 0;
}

/* Redirect all drawing into an offscreen buffer; fb_end_offscreen() copies
 * it to the screen in one pass. Same pitch as the hardware buffer so each
 * dirty row flips with a single memmove. If the allocation fails we silently
 * keep drawing direct - everything still works, it just flickers like
 * before (and the flip below stays a no-op). */
void fb_begin_offscreen(void)
{
    if (back_active) return;
    if (!fb_back) fb_back = (uint8_t *)kmalloc((size_t)fb_h * fb_pitch);
    if (!fb_back) return;
    fb_base = fb_back;
    back_active = 1;
}

void fb_end_offscreen(void)
{
    if (!back_active) return;
    back_active = 0;
    fb_base = fb_hw;

    /* Copy only the dirty rectangle, row by row. dmg_add() already clamped
     * it to the surface; an empty rect means nothing changed this frame.
     * At 1920x1080x32 a full-surface flip is 8+ MB per repaint - a cursor
     * blink now moves a few hundred bytes instead. */
    if (dmg_x0 < dmg_x1 && dmg_y0 < dmg_y1) {
        long   off = (long)dmg_y0 * fb_pitch + (long)dmg_x0 * fb_bytes_per_px;
        size_t run = (size_t)(dmg_x1 - dmg_x0) * fb_bytes_per_px;
        for (int y = dmg_y0; y < dmg_y1; y++, off += fb_pitch)
            kmemmove(fb_hw + off, fb_back + off, run);
    }
    dmg_reset();   /* flushed - dirty again only once something draws */
}

int fb_width(void)  { return fb_w; }
int fb_height(void) { return fb_h; }
int fb_bpp(void)    { return fb_bpp_val; }

color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    color_t v = 0;
    v |= (color_t)(r >> (8 - r_bits)) << r_shift;
    v |= (color_t)(g >> (8 - g_bits)) << g_shift;
    v |= (color_t)(b >> (8 - b_bits)) << b_shift;
    return v;
}

/* Widen one channel back to 8 bits (replicating the high bits into the low
 * ones so full intensity maps to 255, not 248/252 on 16bpp modes). */
static uint8_t unpack_chan(color_t c, int shift, int bits)
{
    unsigned v = (c >> shift) & ((1u << bits) - 1);
    v <<= 8 - bits;
    return (uint8_t)(v | (v >> bits));
}

color_t fb_mix(color_t bg, color_t fg, uint8_t t)
{
    if (t == 0)   return bg;
    if (t == 255) return fg;
    uint8_t r = (uint8_t)(unpack_chan(bg, r_shift, r_bits) +
                (((int)unpack_chan(fg, r_shift, r_bits) - unpack_chan(bg, r_shift, r_bits)) * t) / 255);
    uint8_t g = (uint8_t)(unpack_chan(bg, g_shift, g_bits) +
                (((int)unpack_chan(fg, g_shift, g_bits) - unpack_chan(bg, g_shift, g_bits)) * t) / 255);
    uint8_t b = (uint8_t)(unpack_chan(bg, b_shift, b_bits) +
                (((int)unpack_chan(fg, b_shift, b_bits) - unpack_chan(bg, b_shift, b_bits)) * t) / 255);
    return fb_rgb(r, g, b);
}

void fb_put_pixel(int x, int y, color_t c)
{
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h)
        return;
    dmg_add(x, y, x + 1, y + 1);
    uint8_t *p = fb_base + (long)y * fb_pitch + (long)x * fb_bytes_per_px;
    if (fb_bytes_per_px == 4) {
        *(uint32_t *)p = c;
    } else if (fb_bytes_per_px == 2) {
        *(uint16_t *)p = (uint16_t)c;
    } else {
        p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF;
    }
}

color_t fb_get_pixel(int x, int y)
{
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h)
        return 0;
    uint8_t *p = fb_base + (long)y * fb_pitch + (long)x * fb_bytes_per_px;
    if (fb_bytes_per_px == 4)
        return *(uint32_t *)p;
    if (fb_bytes_per_px == 2)
        return *(uint16_t *)p;
    return (color_t)(p[0] | (p[1] << 8) | (p[2] << 16));
}

void fb_fill_rect(int x, int y, int w, int h, color_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    dmg_add(x, y, x + w, y + h);

    for (int row = 0; row < h; row++) {
        uint8_t *p = fb_base + (long)(y + row) * fb_pitch + (long)x * fb_bytes_per_px;
        if (fb_bytes_per_px == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int col = 0; col < w; col++) q[col] = c;
        } else if (fb_bytes_per_px == 2) {   /* 16bpp rows are 2 bytes/px - a 3-byte stride would overrun */
            uint16_t *q = (uint16_t *)p;
            for (int col = 0; col < w; col++) q[col] = (uint16_t)c;
        } else {
            for (int col = 0; col < w; col++) {
                p[col*3] = c & 0xFF; p[col*3+1] = (c >> 8) & 0xFF; p[col*3+2] = (c >> 16) & 0xFF;
            }
        }
    }
}

void fb_scroll_up(int x, int y, int w, int h, int dy, color_t fill)
{
    if (dy <= 0 || dy >= h) { fb_fill_rect(x, y, w, h, fill); return; }
    dmg_add(x, y, x + w, y + h);   /* fb_fill_rect below covers the strip, this covers the moved rows */
    for (int row = 0; row < h - dy; row++) {
        uint8_t *dst = fb_base + (long)(y + row)      * fb_pitch + (long)x * fb_bytes_per_px;
        uint8_t *src = fb_base + (long)(y + row + dy) * fb_pitch + (long)x * fb_bytes_per_px;
        kmemmove(dst, src, (size_t)w * fb_bytes_per_px);
    }
    fb_fill_rect(x, y + h - dy, w, dy, fill);
}

void fb_blit(int dx, int dy, int w, int h, const color_t *src)
{
    if (!src || w <= 0 || h <= 0) return;

    int src_stride = w;          /* the caller's buffer is unclipped, tightly packed */
    int clip_x = 0, clip_y = 0;  /* how much we chopped off the left/top */
    if (dx < 0) { clip_x = -dx; dx = 0; }
    if (dy < 0) { clip_y = -dy; dy = 0; }

    int cw = w - clip_x, ch = h - clip_y;
    if (dx + cw > fb_w) cw = fb_w - dx;
    if (dy + ch > fb_h) ch = fb_h - dy;
    if (cw <= 0 || ch <= 0) return;
    dmg_add(dx, dy, dx + cw, dy + ch);

    for (int row = 0; row < ch; row++) {
        const color_t *s = src + (long)(clip_y + row) * src_stride + clip_x;
        uint8_t *p = fb_base + (long)(dy + row) * fb_pitch + (long)dx * fb_bytes_per_px;
        if (fb_bytes_per_px == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int col = 0; col < cw; col++) q[col] = s[col];
        } else if (fb_bytes_per_px == 2) {   /* 16bpp rows are 2 bytes/px - a 3-byte stride would overrun */
            uint16_t *q = (uint16_t *)p;
            for (int col = 0; col < cw; col++) q[col] = (uint16_t)s[col];
        } else {
            for (int col = 0; col < cw; col++) {
                color_t c = s[col];
                p[col*3] = c & 0xFF; p[col*3+1] = (c >> 8) & 0xFF; p[col*3+2] = (c >> 16) & 0xFF;
            }
        }
    }
}
