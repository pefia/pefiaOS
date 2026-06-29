/* kernel/explorer.h - the File Explorer app (renders inside a WM window). */
#ifndef PEFIA_EXPLORER_H
#define PEFIA_EXPLORER_H

#include "wm.h"

void explorer_paint(Window *w, int cx, int cy, int cw, int ch);
void explorer_click(Window *w, int lx, int ly);  /* lx,ly are content-relative */
void app_launch_explorer(void);                  /* open a new explorer window  */

#endif /* PEFIA_EXPLORER_H */
