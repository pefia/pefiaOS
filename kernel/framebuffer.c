#include "framebuffer.h"
#include "multiboot.h"
#include "util.h"

static uint8_t *fb_base;
static int fb_w, fb_h;
static int fb_pitch;          /* bytes per scanline */
static int fb_bpp_val, fb_bytes_per_px;

/* Where each color channel lives within a pixel, straight from Multiboot's
 * color_info. */
static int r_shift, r_bits, g_shift, g_bits, b_shift, b_bits;

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
    return 0;
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

void fb_put_pixel(int x, int y, color_t c)
{
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h)
        return;
    uint8_t *p = fb_base + (long)y * fb_pitch + (long)x * fb_bytes_per_px;
    if (fb_bytes_per_px == 4) {
        *(uint32_t *)p = c;
    } else {                 /* 24bpp, no padding byte */
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
    return (color_t)(p[0] | (p[1] << 8) | (p[2] << 16));
}

void fb_fill_rect(int x, int y, int w, int h, color_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        uint8_t *p = fb_base + (long)(y + row) * fb_pitch + (long)x * fb_bytes_per_px;
        if (fb_bytes_per_px == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int col = 0; col < w; col++) q[col] = c;
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

    for (int row = 0; row < ch; row++) {
        const color_t *s = src + (long)(clip_y + row) * src_stride + clip_x;
        uint8_t *p = fb_base + (long)(dy + row) * fb_pitch + (long)dx * fb_bytes_per_px;
        if (fb_bytes_per_px == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int col = 0; col < cw; col++) q[col] = s[col];
        } else {
            for (int col = 0; col < cw; col++) {
                color_t c = s[col];
                p[col*3] = c & 0xFF; p[col*3+1] = (c >> 8) & 0xFF; p[col*3+2] = (c >> 16) & 0xFF;
            }
        }
    }
}
