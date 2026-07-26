#ifndef PEFIA_INPUT_H
#define PEFIA_INPUT_H

void input_init(void);     /* enable the PS/2 mouse; keyboard is already on */
int  input_getchar(void);  /* block for a key; moves the cursor meanwhile */
int  input_poll(void);     /* non-blocking: service pending bytes, return a
                              key char if one was pressed this call, else 0 */

/* Non-ASCII keys delivered through the char stream above. The range
 * 0x81..0x8F is reserved for these: it can't collide with printable ASCII
 * (32..126), and as a signed char every value is negative, so consumers
 * that only insert 32..126 ignore them without changes. */
enum {
    KEY_UP    = 0x81,
    KEY_DOWN  = 0x82,
    KEY_LEFT  = 0x83,
    KEY_RIGHT = 0x84,
};

/* Real-time key state, for games.
 * The char stream above is fine for typing but useless for held keys. These
 * two calls run alongside it: handle_key() keeps a pressed/released bitmap
 * for the handful of keys anything game-like cares about.
 *   input_key_down    - is the key held right now?
 *   input_key_pressed - did it go down since the last query? (latched + cleared)
 */
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

/* Raw key events, for DOOM (which wants a down+up pair for every key rather
 * than the debounced state above). Each make/break code is queued as
 * (scancode, pressed, ext); ext is 1 for 0xE0-prefixed keys (arrows, right
 * ctrl/alt). Returns 1 and fills the outputs if an event was dequeued, 0 if
 * the queue is empty. Gets filled as a side effect of input_poll() /
 * input_getchar() draining the controller - there's no separate pump. */
int  input_next_event(int *scancode, int *pressed, int *ext);

#endif
