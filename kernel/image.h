#ifndef PEFIA_IMAGE_H
#define PEFIA_IMAGE_H

#include <stdint.h>
#include "bitmap.h"

/* Detects PNG/GIF/JPEG/BMP from the header and decodes into *out.
 * On success out->pixels is heap-allocated (release with bmp_free) and
 * holds ARGB8888 pixels. Decoders reject absurd dimensions up front so a
 * malicious or corrupt image can't be used to blow the heap. Returns 0
 * on success, negative on any parse failure. */
int img_decode(const uint8_t *data, int len, Bitmap *out);

/* Animation support. Only GIF can actually animate; every other format
 * (and a single-frame GIF) just fills one frame so callers can treat all
 * images uniformly. */
#define ANIM_MAX_FRAMES 16
typedef struct {
    Bitmap frames[ANIM_MAX_FRAMES];   /* each independently kmalloc'd; free all via anim_free */
    int    delays_ms[ANIM_MAX_FRAMES];
    int    count;
} AnimBitmap;

/* Decodes any supported format. Non-GIF and single-frame GIF produce count==1
 * with delays_ms[0]==0. Animated GIF: up to ANIM_MAX_FRAMES composited frames
 * (later frames beyond the cap are ignored). Returns 0 on success. */
int  img_decode_anim(const uint8_t *data, int len, AnimBitmap *out);
void anim_free(AnimBitmap *a);

#endif
