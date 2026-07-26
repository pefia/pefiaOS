#ifndef PEFIA_BROWSER_H
#define PEFIA_BROWSER_H

#include "wm.h"

void *browser_new(void);
void  browser_paint(Window *w, int x, int y, int wdt, int hgt);
void  browser_key(Window *w, char c);
void  browser_click(Window *w, int relx, int rely);
void  browser_scroll(Window *w, int notches);
void  browser_goto(Window *w, const char *url);
/* Non-blocking pump: runs script timers and pending load handlers, acts on
 * anything they asked for (navigation, alert), and returns 1 when the window
 * needs repainting - a page whose script or animation changed it gets no
 * input event to trigger the repaint on its own. */
int   browser_tick(Window *w);

#endif
