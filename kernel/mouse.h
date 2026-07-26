#ifndef PEFIA_MOUSE_H
#define PEFIA_MOUSE_H

void     mouse_cursor_init(int x, int y);
void     mouse_hide(void);
void     mouse_show(void);
void     mouse_move(int dx, int dy);       /* move by a delta, clamp to screen */
void     mouse_set_buttons(unsigned buttons);
unsigned mouse_buttons(void);
int      mouse_x(void);
int      mouse_y(void);

/* Scroll wheel (only fed when the mouse speaks the 4-byte IntelliMouse
 * protocol - see input.c). Positive = wheel rolled toward the user, i.e.
 * scroll down. take_wheel returns the notches accumulated since the last
 * call and resets the count. */
void     mouse_add_wheel(int notches);
int      mouse_take_wheel(void);

#endif
