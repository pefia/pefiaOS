/* kernel/games.h
 * One dispatch table, seven games - Flappy Bird, Pong, a Mario-style platformer,
 * a Wolfenstein-style raycaster maze, Tetris, Snake, and Breakout - so the
 * window manager only needs a single WIN_GAME case instead of one per game.
 *
 * Every game renders into an off-screen buffer and hands it to fb_blit() in
 * one shot, which is what keeps them flicker-free. Input is read straight from
 * input.h's key-state table (WASD or arrows; space covers flap/jump/hard-drop
 * and also restarts after a game over). */
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

void *game_new(int kind);                  /* allocate state; buffer sized lazily */
void  game_free(void *state);              /* free buffer + state (WM close hook) */

void  game_paint(Window *w, int cx, int cy, int cw, int ch);   /* redraw current frame */
void  game_tick (Window *w, int cx, int cy, int cw, int ch);   /* advance + present     */
void  game_key  (Window *w, char c);                           /* 'r' restarts          */
void  game_resize(Window *w, int cw, int ch);                  /* reallocate buffer     */

#endif /* PEFIA_GAMES_H */
