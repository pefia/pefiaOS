/* Software mouse cursor. We track a position and draw an arrow straight
 * onto the framebuffer, saving whatever was underneath first so hiding it
 * again doesn't leave a trail. The actual PS/2 byte-wrangling lives in
 * input.c - it calls mouse_move() whenever a packet comes in. */
#ifndef PEFIA_MOUSE_H
#define PEFIA_MOUSE_H

void     mouse_cursor_init(int x, int y);  /* set start position, enable, draw */
void     mouse_hide(void);                 /* restore pixels under the cursor */
void     mouse_show(void);                 /* save pixels, draw the cursor */
void     mouse_move(int dx, int dy);       /* move by a delta, clamp to screen */
void     mouse_set_buttons(unsigned buttons);
unsigned mouse_buttons(void);              /* current button bitmask (bit0=left) */
int      mouse_x(void);
int      mouse_y(void);

#endif /* PEFIA_MOUSE_H */
