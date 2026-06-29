/* kernel/framebuffer.c */
#include "framebuffer.h"
#include "multiboot.h"
#include "util.h"

static uint8_t *fb;          /* base address of the framebuffer */
static int width, height;
static int pitch;            /* bytes per scanline */
static int bpp, bytes;       /* bits / bytes per pixel */

/* Pixel channel positions/sizes (from the Multiboot color_info). */
static int r_pos, r_sz, g_pos, g_sz, b_pos, b_sz;

int fb_init(uint32_t magic, uint32_t info_addr)
{
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        return -1;

    struct multiboot_info *mb = (struct multiboot_info *)info_addr;
    if (!(mb->flags & MULTIBOOT_INFO_FRAMEBUFFER))
        return -1;
    if (mb->framebuffer_type != 1)   /* 1 = direct RGB; we don't do palettes */
        return -1;

    fb     = (uint8_t *)(uint32_t)mb->framebuffer_addr;
    width  = (int)mb->framebuffer_width;
    height = (int)mb->framebuffer_height;
    pitch  = (int)mb->framebuffer_pitch;
    bpp    = mb->framebuffer_bpp;
    bytes  = bpp / 8;

    r_pos = mb->color_info[0]; r_sz = mb->color_info[1];
    g_pos = mb->color_info[2]; g_sz = mb->color_info[3];
    b_pos = mb->color_info[4]; b_sz = mb->color_info[5];

    /* GRUB's emulated VBE often reports incomplete masks (e.g. red size 0).
     * Trust color_info only if every channel size is sane; otherwise use the
     * standard layout for this depth (32/24bpp = XRGB8888, 16bpp = RGB565). */
    if (!(r_sz && g_sz && b_sz)) {
        if (bpp >= 24) {
            r_pos = 16; r_sz = 8; g_pos = 8; g_sz = 8; b_pos = 0; b_sz = 8;
        } else {
            r_pos = 11; r_sz = 5; g_pos = 5; g_sz = 6; b_pos = 0; b_sz = 5;
        }
    }
    return 0;
}

int fb_width(void)  { return width; }
int fb_height(void) { return height; }
int fb_bpp(void)    { return bpp; }

color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    color_t v = 0;
    v |= (color_t)(r >> (8 - r_sz)) << r_pos;
    v |= (color_t)(g >> (8 - g_sz)) << g_pos;
    v |= (color_t)(b >> (8 - b_sz)) << b_pos;
    return v;
}

void fb_put_pixel(int x, int y, color_t c)
{
    if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height)
        return;
    uint8_t *p = fb + (long)y * pitch + (long)x * bytes;
    if (bytes == 4) {
        *(uint32_t *)p = c;
    } else {                 /* 24bpp */
        p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF;
    }
}

color_t fb_get_pixel(int x, int y)
{
    if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height)
        return 0;
    uint8_t *p = fb + (long)y * pitch + (long)x * bytes;
    if (bytes == 4)
        return *(uint32_t *)p;
    return (color_t)(p[0] | (p[1] << 8) | (p[2] << 16));
}

void fb_fill_rect(int x, int y, int w, int h, color_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width)  w = width  - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        uint8_t *p = fb + (long)(y + j) * pitch + (long)x * bytes;
        if (bytes == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int i = 0; i < w; i++) q[i] = c;
        } else {
            for (int i = 0; i < w; i++) {
                p[i*3] = c & 0xFF; p[i*3+1] = (c >> 8) & 0xFF; p[i*3+2] = (c >> 16) & 0xFF;
            }
        }
    }
}

void fb_scroll_up(int x, int y, int w, int h, int dy, color_t fill)
{
    if (dy <= 0 || dy >= h) { fb_fill_rect(x, y, w, h, fill); return; }
    for (int j = 0; j < h - dy; j++) {
        uint8_t *dst = fb + (long)(y + j)      * pitch + (long)x * bytes;
        uint8_t *src = fb + (long)(y + j + dy) * pitch + (long)x * bytes;
        kmemmove(dst, src, (size_t)w * bytes);
    }
    fb_fill_rect(x, y + h - dy, w, dy, fill);
}

void fb_blit(int dx, int dy, int w, int h, const color_t *src)
{
    if (!src || w <= 0 || h <= 0) return;

    int stride = w;            /* source row stride is the unclipped width */
    int ox = 0, oy = 0;        /* offset into the source after left/top clip */
    if (dx < 0) { ox = -dx; dx = 0; }
    if (dy < 0) { oy = -dy; dy = 0; }

    int cw = w - ox, ch = h - oy;
    if (dx + cw > width)  cw = width  - dx;
    if (dy + ch > height) ch = height - dy;
    if (cw <= 0 || ch <= 0) return;

    for (int j = 0; j < ch; j++) {
        const color_t *s = src + (long)(oy + j) * stride + ox;
        uint8_t *p = fb + (long)(dy + j) * pitch + (long)dx * bytes;
        if (bytes == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int i = 0; i < cw; i++) q[i] = s[i];
        } else {
            for (int i = 0; i < cw; i++) {
                color_t c = s[i];
                p[i*3] = c & 0xFF; p[i*3+1] = (c >> 8) & 0xFF; p[i*3+2] = (c >> 16) & 0xFF;
            }
        }
    }
}
