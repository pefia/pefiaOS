/* kernel/games.h
 * -----------------------------------------------------------------------------
 * Five built-in games, all driven through one uniform app interface so the
 * window manager only needs a single WIN_GAME branch:
 *
 *   GAME_FLAPPY   - Flappy Bird   (tap to flap)
 *   GAME_PONG     - Pong          (you vs. the CPU)
 *   GAME_MARIO    - a Mario-style platform level (run, jump, stomp, flag)
 *   GAME_RAYCAST  - Maze 3D, a Wolfenstein-style raycaster (real 3D rendering)
 *   GAME_TETRIS   - Tetris        (the seven tetrominoes, line clears)
 *
 * Each game composes its frame into an off-screen buffer and presents it with
 * fb_blit() for flicker-free animation. Controls use the real-time key-state
 * API in input.h (WASD or arrow keys; space = flap/jump/hard-drop/restart).
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_GAMES_H
#define PEFIA_GAMES_H

#include "wm.h"

typedef enum {
    GAME_FLAPPY = 0,
    GAME_PONG,
    GAME_MARIO,
    GAME_RAYCAST,
    GAME_TETRIS,
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
