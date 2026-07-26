#ifndef STUB_FB
#define STUB_FB
#include <stdint.h>
typedef uint32_t color_t;
static inline color_t fb_rgb(uint8_t r,uint8_t g,uint8_t b){return (color_t)(r|g|b);}
static inline void fb_fill_rect(int a,int b,int c,int d,color_t e){(void)a;(void)b;(void)c;(void)d;(void)e;}
#endif
