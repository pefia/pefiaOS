/* kernel/taskbar.h - the bottom bar: Start menu + search, window buttons, clock. */
#ifndef PEFIA_TASKBAR_H
#define PEFIA_TASKBAR_H

void taskbar_paint(void);
int  taskbar_click(int mx, int my);   /* returns 1 if it swallowed the click */
int  taskbar_menu_open(void);
void taskbar_key(char c);             /* feed a key to the search box */

#endif /* PEFIA_TASKBAR_H */
