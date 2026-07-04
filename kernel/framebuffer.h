/* Linear framebuffer driver. GRUB hands us the address, pitch, dimensions
 * and pixel format through the Multiboot info struct - we just read it
 * once at boot and poke pixels into it from then on. */
#ifndef PEFIA_FRAMEBUFFER_H
#define PEFIA_FRAMEBUFFER_H

#include <stdint.h>

typedef uint32_t color_t;   /* packed in the hardware's native pixel format */

/* Returns 0 on success, -1 if no usable RGB framebuffer was provided. */
int fb_init(uint32_t mb_magic, uint32_t mb_info_addr);

int fb_width(void);
int fb_height(void);
int fb_bpp(void);

/* Build a native pixel value from 8-bit R/G/B (handles BGR vs RGB ordering). */
color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void    fb_put_pixel(int x, int y, color_t c);
color_t fb_get_pixel(int x, int y);
void    fb_fill_rect(int x, int y, int w, int h, color_t c);

/* Shift a rectangle up by dy pixels and fill the exposed strip with `fill`. */
void    fb_scroll_up(int x, int y, int w, int h, int dy, color_t fill);

/* Blit a w*h block of native-format pixels (row-major, stride == w) to the
 * screen at (x,y), clipped to the framebuffer. Lets apps compose a frame
 * off-screen and present it in one pass (flicker-free animation). */
void    fb_blit(int x, int y, int w, int h, const color_t *src);

#endif /* PEFIA_FRAMEBUFFER_H */
