/* kernel/mouse.h
 * -----------------------------------------------------------------------------
 * Software mouse cursor: tracks a position and draws an arrow on the
 * framebuffer, saving/restoring the pixels underneath so it doesn't smear.
 * (The PS/2 hardware reading lives in input.c, which calls mouse_move.)
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_MOUSE_H
#define PEFIA_MOUSE_H

void     mouse_cursor_init(int x, int y);  /* set start position, enable, draw */
void     mouse_hide(void);                 /* restore pixels under the cursor */
void     mouse_show(void);                 /* save pixels, draw the cursor */
void     mouse_move(int dx, int dy);       /* move by a delta, clamp to screen */
void     mouse_set_buttons(unsigned buttons);
unsigned mouse_buttons(void);              /* current button bitmask (bit0=left) */
int      mouse_x(void);
int      mouse_y(void);          /* boi this is so freakin tuff*/

#endif /* PEFIA_MOUSE_H */
