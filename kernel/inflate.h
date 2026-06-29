/* kernel/inflate.h - DEFLATE (RFC1951) decoder + zlib/gzip wrappers. */
#ifndef PEFIA_INFLATE_H
#define PEFIA_INFLATE_H

#include <stdint.h>

/* Each returns the number of output bytes written, or <0 on error. */
int inflate_raw(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);
int zlib_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);
int gzip_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);

/* Sniff + inflate: handles gzip (1f 8b), zlib (78 xx), else raw deflate. */
int auto_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);

#endif /* PEFIA_INFLATE_H */
