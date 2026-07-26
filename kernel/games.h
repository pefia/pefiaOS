#ifndef PEFIA_GAMES_H
#define PEFIA_GAMES_H

#include "wm.h"

typedef enum {
    GAME_FLAPPY = 0,
    GAME_PONG,
    GAME_MARIO,
    GAME_RAYCAST,
    GAME_TETRIS,
    GAME_SNAKE,
    GAME_BREAKOUT,
    GAME_COUNT
} GameKind;

const char *game_title(int kind);

void *game_new(int kind);
void  game_free(void *state);              /* free buffer + state (WM close hook) */

void  game_paint(Window *w, int cx, int cy, int cw, int ch);
void  game_tick (Window *w, int cx, int cy, int cw, int ch);
void  game_key  (Window *w, char c);
void  game_resize(Window *w, int cw, int ch);

#endif
