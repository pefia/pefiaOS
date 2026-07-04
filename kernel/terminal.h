/* kernel/terminal.h - a shell that runs inside a window instead of owning the whole screen. */
#ifndef PEFIA_TERMINAL_H
#define PEFIA_TERMINAL_H

#include "wm.h"

void *terminal_new(void);
void  terminal_paint(Window *w, int cx, int cy, int cw, int ch);
void  terminal_key(Window *w, char c);

#endif /* PEFIA_TERMINAL_H */
