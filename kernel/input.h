/* kernel/input.h
 * -----------------------------------------------------------------------------
 * Unified polling driver for the PS/2 keyboard and mouse. A single status
 * register tells us which device a byte came from, so one loop services both.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_INPUT_H
#define PEFIA_INPUT_H

void input_init(void);     /* enable the PS/2 mouse; keyboard is already on */
int  input_getchar(void);  /* block for a key; moves the cursor meanwhile */
int  input_poll(void);     /* non-blocking: service pending bytes, return a
                              key char if one was pressed this call, else 0 */

/* ---- real-time key state (for games) --------------------------------------
 * The char stream above is fine for typing but useless for held keys, and it
 * drops the arrow keys entirely. These run alongside it: handle_key() records a
 * pressed/released bitmap for the handful of keys games care about. Arrow keys
 * (PS/2 extended, 0xE0-prefixed) are reported *only* through this API.
 *   input_key_down    - is the key held right now?
 *   input_key_pressed - did it go down since the last query? (latched + cleared)
 * ------------------------------------------------------------------------- */
#define IK_LEFT  0
#define IK_RIGHT 1
#define IK_UP    2
#define IK_DOWN  3
#define IK_SPACE 4
#define IK_W     5
#define IK_A     6
#define IK_S     7
#define IK_D     8
#define IK_COUNT 9

int  input_key_down(int code);
int  input_key_pressed(int code);

/* ---- raw key events (for DOOM, which needs down+up for every key) ----------
 * Each make/break the keyboard produces is queued as (scancode, pressed, ext).
 * `ext` is 1 for 0xE0-prefixed keys (arrows, right ctrl/alt). Returns 1 and
 * fills the outputs if an event was dequeued, else 0. The queue is filled as a
 * side effect of input_poll()/input_getchar() draining the controller. */
int  input_next_event(int *scancode, int *pressed, int *ext);

#endif /* PEFIA_INPUT_H */
