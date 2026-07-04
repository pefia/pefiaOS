/* Scrolling text console drawn onto a rectangle of the framebuffer using
 * the embedded 8x16 bitmap font. Think "a terminal", minus the terminal -
 * just character cells and a cursor. */
#ifndef PEFIA_CONSOLE_H
#define PEFIA_CONSOLE_H

#include "framebuffer.h"

/* Place the console at pixel (x,y), `cols` x `rows` character cells. */
void con_init(int x, int y, int cols, int rows, color_t fg, color_t bg);
void con_clear(void);
void con_setcolor(color_t fg, color_t bg);
void con_putchar(char c);                 /* handles \n \r \t \b + scrolling */
void con_write(const char *s);

/* Draw a string directly at a pixel position (used for window title bars). */
void gfx_text(int x, int y, const char *s, color_t fg, color_t bg);

#endif /* PEFIA_CONSOLE_H */
