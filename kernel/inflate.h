/* kernel/inflate.h
 *
 * DEFLATE (RFC 1951) decompressor plus the two container formats that
 * wrap it in practice: zlib (RFC 1950) and gzip (RFC 1952). PNG uses
 * zlib framing; HTTP responses tend to show up as either raw deflate
 * or gzip, hence auto_inflate() below for callers that don't want to
 * care which.
 */
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

#endif /* PEFIA_INFLATE_H */
