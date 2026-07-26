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

/* Blend fg over bg per channel: t=0 gives bg, t=255 gives fg. Used for
 * antialiased text, where t is the glyph pixel's coverage. */
color_t fb_mix(color_t bg, color_t fg, uint8_t t);

void    fb_put_pixel(int x, int y, color_t c);
color_t fb_get_pixel(int x, int y);
void    fb_fill_rect(int x, int y, int w, int h, color_t c);

/* Shift a rectangle up by dy pixels and fill the exposed strip with `fill`. */
void    fb_scroll_up(int x, int y, int w, int h, int dy, color_t fill);

/* Blit a w*h block of native-format pixels (row-major, stride == w) to the
 * screen at (x,y), clipped to the framebuffer. Lets apps compose a frame
 * off-screen and present it in one pass (flicker-free animation). */
void    fb_blit(int x, int y, int w, int h, const color_t *src);

/* Between begin and end, every fb_* draw above lands in an offscreen buffer;
 * end copies just the region that was drawn to (tracked internally) to the
 * screen in one pass. The window manager wraps each full repaint in this
 * pair so dragging/resizing doesn't flicker. */
void    fb_begin_offscreen(void);
void    fb_end_offscreen(void);

/* Mark the whole surface dirty, so the next fb_end_offscreen() copies all
 * of it. For callers that wrote pixels behind the driver's back. */
void    fb_damage_all(void);

#endif
