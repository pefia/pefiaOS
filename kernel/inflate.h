#ifndef PEFIA_INFLATE_H
#define PEFIA_INFLATE_H

#include <stdint.h>

/* All four return the number of bytes written to dst, or a negative
 * value if the stream is malformed or dst is too small to hold the
 * result. None of them allocate - dst/dstcap must already be sized
 * for the expected output. */
int inflate_raw(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);
int zlib_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);
int gzip_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);

/* Looks at the first couple of bytes to guess the container (gzip
 * magic 1f 8b, or a zlib header whose CMF/FLG pair is divisible by
 * 31) and dispatches accordingly. Falls back to raw deflate. */
int auto_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);

#endif
