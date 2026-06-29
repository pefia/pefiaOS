/* kernel/util.h
 * -----------------------------------------------------------------------------
 * Tiny freestanding replacements for the libc functions we miss. Header-only.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_UTIL_H
#define PEFIA_UTIL_H

#include <stdint.h>
#include <stddef.h>

static inline void *kmemset(void *dst, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

static inline void *kmemmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

static inline size_t kstrlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline char *kstrcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}

static inline char *kstrcat(char *d, const char *s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) { }
    return r;
}

/* Unsigned int -> decimal string. buf must hold at least 11 bytes. */
static inline char *kutoa(uint32_t v, char *buf)
{
    char tmp[11];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

#endif /* PEFIA_UTIL_H */
