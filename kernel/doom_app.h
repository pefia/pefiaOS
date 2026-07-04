/* kernel/doom_app.h
 * Glue between the window manager and PureDOOM. See doom_app.c for the
 * gory details of how a single-header C89 DOOM ends up drawing into a
 * pefiaOS window.
 */
#ifndef PEFIA_DOOM_APP_H
#define PEFIA_DOOM_APP_H

#include "wm.h"

void *doom_app_new(void);
void  doom_app_free(void *state);
void  doom_app_paint(Window *w, int cx, int cy, int cw, int ch);
void  doom_app_tick(Window *w, int cx, int cy, int cw, int ch);
void  doom_app_resize(Window *w, int cw, int ch);

#endif /* PEFIA_DOOM_APP_H */
