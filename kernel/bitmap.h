#ifndef PEFIA_BITMAP_H
#define PEFIA_BITMAP_H

#include <stdint.h>
#include "framebuffer.h"

typedef struct {
    int width;
    int height;
    int stride;
    int has_alpha;
    uint32_t *pixels; /* ARGB8888 */
} Bitmap;

int  bmp_decode(const uint8_t *data, int len, Bitmap *out);
void bmp_free(Bitmap *bmp);
void bmp_blit(const Bitmap *bmp, int dst_x, int dst_y, int max_w, int max_h);

#endif /* PEFIA_BITMAP_H */
