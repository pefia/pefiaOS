#ifndef PEFIA_THEME_H
#define PEFIA_THEME_H

#include "framebuffer.h"
#include <stdint.h>

color_t theme_accent(void);
void    theme_accent_rgb(uint8_t *r, uint8_t *g, uint8_t *b);
void    theme_set_accent(int r, int g, int b);

int     theme_dark(void);
void    theme_set_dark(int d);

#endif
