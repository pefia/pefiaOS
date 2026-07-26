#include "theme.h"

static int accent_r = 0, accent_g = 120, accent_b = 215;
static int dark_mode = 1;

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void theme_set_accent(int r, int g, int b)
{
    accent_r = clamp8(r); accent_g = clamp8(g); accent_b = clamp8(b);
}

void theme_accent_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)accent_r; *g = (uint8_t)accent_g; *b = (uint8_t)accent_b;
}

color_t theme_accent(void) { return fb_rgb((uint8_t)accent_r, (uint8_t)accent_g, (uint8_t)accent_b); }

int  theme_dark(void) { return dark_mode; }
void theme_set_dark(int d) { dark_mode = d ? 1 : 0; }
