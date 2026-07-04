/* kernel/browser.h - toolbar, address bar, history, and a viewport onto
 * whatever htmlrender.c makes of the current page. */
#ifndef PEFIA_BROWSER_H
#define PEFIA_BROWSER_H

#include "wm.h"

void *browser_new(void);
void  browser_paint(Window *w, int x, int y, int wdt, int hgt);
void  browser_key(Window *w, char c);
void  browser_click(Window *w, int relx, int rely);
void  browser_goto(Window *w, const char *url);   /* navigate programmatically */
void  browser_tick(Window *w);                    /* non-blocking resource pump */

#endif /* PEFIA_BROWSER_H */
