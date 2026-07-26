#ifndef PEFIA_WM_H
#define PEFIA_WM_H

typedef enum { WIN_INFO, WIN_EXPLORER, WIN_TERMINAL, WIN_NOTEPAD, WIN_BROWSER, WIN_GAME, WIN_DOOM, WIN_INTERP, WIN_SETTINGS } WinKind;

typedef struct Window {
    int      x, y, w, h;
    char     title[64];
    char     body[96];
    WinKind  kind;
    int      cwd, sel;
    void    *state;
    int      visible;
    int      snapped;        /* 1 when edge-tiled; sx..sh hold the pre-snap rect */
    int      sx, sy, sw, sh;
} Window;

void    wm_init(void);
Window *wm_create_info(int x, int y, int w, int h, const char *title, const char *body);
Window *wm_create_explorer(int x, int y, int w, int h, const char *title, int dir);
Window *wm_create_terminal(int x, int y, int w, int h);
Window *wm_create_notepad(int x, int y, int w, int h);
Window *wm_create_editor(int x, int y, int w, int h, int dir, int node);
Window *wm_create_browser(int x, int y, int w, int h);
Window *wm_create_game(int kind, int x, int y, int w, int h);
Window *wm_create_doom(int x, int y, int w, int h);
Window *wm_create_interp(int lang, int x, int y, int w, int h);
Window *wm_create_settings(int x, int y, int w, int h);
void    wm_run(void);

int         wm_count(void);
const char *wm_title(int i);
void        wm_raise(int i);

#endif
