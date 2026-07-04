/* kernel/image.h
 *
 * One entry point for turning compressed image bytes into a Bitmap the
 * rest of the kernel can blit. Format is sniffed from the magic bytes,
 * not from a file extension or caller hint - useful since most callers
 * here got the data over HTTP with no filename attached.
 */
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

#endif /* PEFIA_IMAGE_H */
