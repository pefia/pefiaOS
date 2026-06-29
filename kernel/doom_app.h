/* kernel/doom_app.h - id Software's DOOM as a pefiaOS window app (via PureDOOM). */
#ifndef PEFIA_DOOM_APP_H
#define PEFIA_DOOM_APP_H

#include "wm.h"

void *doom_app_new(void);
void  doom_app_free(void *state);
void  doom_app_paint(Window *w, int cx, int cy, int cw, int ch);
void  doom_app_tick(Window *w, int cx, int cy, int cw, int ch);
void  doom_app_resize(Window *w, int cw, int ch);

#endif /* PEFIA_DOOM_APP_H */
