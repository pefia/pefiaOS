/* kernel/notepad.h - a minimal text editor app. */
#ifndef PEFIA_NOTEPAD_H
#define PEFIA_NOTEPAD_H

#include "wm.h"

void *notepad_new(void);
void  notepad_paint(Window *w, int cx, int cy, int cw, int ch);
void  notepad_key(Window *w, char c);

#endif /* PEFIA_NOTEPAD_H */
