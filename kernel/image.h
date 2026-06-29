/* kernel/image.h - decode PNG/GIF/JPEG/BMP into an ARGB8888 Bitmap. */
#ifndef PEFIA_IMAGE_H
#define PEFIA_IMAGE_H

#include <stdint.h>
#include "bitmap.h"

/* Sniff the format from the header bytes and decode. Returns 0 on success
 * (out->pixels is kmalloc'd; free with bmp_free). Bounds source pixels so a
 * crafted image can't exhaust the heap. */
int img_decode(const uint8_t *data, int len, Bitmap *out);

#endif /* PEFIA_IMAGE_H */
