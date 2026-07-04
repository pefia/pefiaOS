/* kernel/wm.h
 * The window manager's idea of a window: a rect, a title, a kind, and a
 * void* for whatever that kind of app needs to remember between frames. */
#ifndef PEFIA_WM_H
#define PEFIA_WM_H

typedef enum { WIN_INFO, WIN_EXPLORER, WIN_TERMINAL, WIN_NOTEPAD, WIN_BROWSER, WIN_GAME, WIN_DOOM } WinKind;

typedef struct Window {
    int      x, y, w, h;     /* h includes the title bar */
    char     title[64];
    char     body[96];       /* WIN_INFO text */
    WinKind  kind;
    int      cwd, sel;       /* WIN_EXPLORER */
    void    *state;          /* WIN_TERMINAL / WIN_NOTEPAD */
    int      visible;
} Window;

void    wm_init(void);
Window *wm_create_info(int x, int y, int w, int h, const char *title, const char *body);
Window *wm_create_explorer(int x, int y, int w, int h, const char *title, int dir);
Window *wm_create_terminal(int x, int y, int w, int h);
Window *wm_create_notepad(int x, int y, int w, int h);
Window *wm_create_browser(int x, int y, int w, int h);
Window *wm_create_game(int kind, int x, int y, int w, int h);
Window *wm_create_doom(int x, int y, int w, int h);
void    wm_run(void);

int         wm_count(void);
const char *wm_title(int i);
void        wm_raise(int i);

#endif /* PEFIA_WM_H */


