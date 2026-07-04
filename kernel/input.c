#include "input.h"
#include "io.h"
#include "mouse.h"

#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_STAT 0x64

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

/* US QWERTY, scan code set 1. 0 means "no printable character". */
static const char keymap[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'','`', 0,   '\\','z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,
};

static const char keymap_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,
};

/* --- talking to the 8042 controller --- */

static void ps2_wait_input_clear(void)  { int t = 100000; while ((inb(PS2_STAT) & 2) && t--) { } }
static void ps2_wait_output_ready(void) { int t = 100000; while (!(inb(PS2_STAT) & 1) && t--) { } }

static void mouse_command(uint8_t cmd)
{
    ps2_wait_input_clear(); outb(PS2_STAT, 0xD4);   /* next byte goes to the mouse */
    ps2_wait_input_clear(); outb(PS2_DATA, cmd);
    ps2_wait_output_ready(); (void)inb(PS2_DATA);    /* eat the 0xFA ack */
}

void input_init(void)
{
    while (inb(PS2_STAT) & 1) (void)inb(PS2_DATA);   /* flush anything stale */

    ps2_wait_input_clear(); outb(PS2_STAT, 0xA8);   /* enable the aux (mouse) port */

    ps2_wait_input_clear(); outb(PS2_STAT, 0x20);   /* read controller config byte */
    ps2_wait_output_ready(); uint8_t cfg = inb(PS2_DATA);
    cfg |=  0x02;                      /* enable IRQ12 - harmless while we're polling anyway */
    cfg &= ~0x20;                      /* clear the mouse-clock-disabled bit */
    ps2_wait_input_clear(); outb(PS2_STAT, 0x60);   /* write it back */
    ps2_wait_input_clear(); outb(PS2_DATA, cfg);

    mouse_command(0xF6);              /* reset to defaults */
    mouse_command(0xF4);              /* turn on data reporting */
}

/* --- held-key state for games; see input.h --- */
static uint8_t key_held[IK_COUNT];
static uint8_t key_latch[IK_COUNT];

/* Maps a (possibly 0xE0-extended) scan code to an IK_* slot, -1 if we don't
 * track that key. */
static int ik_slot_for(uint8_t code, int ext)
{
    if (ext) {
        switch (code) {
        case 0x4B: return IK_LEFT;
        case 0x4D: return IK_RIGHT;
        case 0x48: return IK_UP;
        case 0x50: return IK_DOWN;
        }
        return -1;
    }
    switch (code) {
    case 0x39: return IK_SPACE;
    case 0x11: return IK_W;
    case 0x1E: return IK_A;
    case 0x1F: return IK_S;
    case 0x20: return IK_D;
    }
    return -1;
}

int input_key_down(int code)
{
    return (code >= 0 && code < IK_COUNT) ? key_held[code] : 0;
}

int input_key_pressed(int code)
{
    if (code < 0 || code >= IK_COUNT) return 0;
    int was_set = key_latch[code];
    key_latch[code] = 0;
    return was_set;
}

/* --- raw key-event ring for DOOM; see input.h --- */
#define EVQ_SIZE 128
static uint8_t evq_code[EVQ_SIZE], evq_press[EVQ_SIZE], evq_ext[EVQ_SIZE];
static int     evq_head, evq_tail;

static void evq_push(uint8_t code, uint8_t pressed, uint8_t ext)
{
    int next = (evq_head + 1) % EVQ_SIZE;
    if (next == evq_tail) return;   /* queue's full, just drop it */
    evq_code[evq_head]  = code;
    evq_press[evq_head] = pressed;
    evq_ext[evq_head]   = ext;
    evq_head = next;
}

int input_next_event(int *scancode, int *pressed, int *ext)
{
    if (evq_tail == evq_head) return 0;
    *scancode = evq_code[evq_tail];
    *pressed  = evq_press[evq_tail];
    *ext      = evq_ext[evq_tail];
    evq_tail = (evq_tail + 1) % EVQ_SIZE;
    return 1;
}

static int handle_key(uint8_t sc)
{
    static int shift_held = 0;
    static int pending_ext = 0;

    if (sc == 0xE0) { pending_ext = 1; return 0; }   /* prefix byte, wait for the next one */

    int     is_release = sc & 0x80;
    uint8_t code    = sc & 0x7F;
    int     was_ext = pending_ext;
    pending_ext = 0;

    /* Anyone consuming raw down/up events wants this regardless of what
     * the debounced state tracking below decides to do with it. */
    evq_push(code, is_release ? 0 : 1, (uint8_t)was_ext);

    /* Typematic repeat resends the make code without a break in between,
     * so only latch a fresh "pressed" edge the first time we see it go
     * down from a released state. */
    int slot = ik_slot_for(code, was_ext);
    if (slot >= 0) {
        if (is_release) key_held[slot] = 0;
        else { if (!key_held[slot]) key_latch[slot] = 1; key_held[slot] = 1; }
    }

    if (is_release) {
        if (code == SC_LSHIFT || code == SC_RSHIFT) shift_held = 0;
        return 0;
    }
    if (code == SC_LSHIFT || code == SC_RSHIFT) { shift_held = 1; return 0; }
    if (was_ext) return 0;    /* arrows etc: only exposed through the state API */

    char c = shift_held ? keymap_shift[code] : keymap[code];
    return c ? (int)(unsigned char)c : 0;
}

static void handle_mouse(uint8_t byte)
{
    static int phase = 0;
    static int packet[3];

    switch (phase) {
    case 0:
        if (!(byte & 0x08)) return;   /* bit3 should always be set on byte 0 - resync */
        packet[0] = byte; phase = 1;
        break;
    case 1:
        packet[1] = byte; phase = 2;
        break;
    default: {
        packet[2] = byte; phase = 0;
        int flags = packet[0];
        if (flags & 0xC0) break;      /* overflow flagged, toss this packet */
        int dx = packet[1] - ((flags << 4) & 0x100);
        int dy = packet[2] - ((flags << 3) & 0x100);
        mouse_set_buttons((unsigned)(flags & 0x07));
        mouse_move(dx, -dy);          /* PS/2 Y is up-positive, screen Y is down-positive */
        break;
    }
    }
}

int input_getchar(void)
{
    for (;;) {
        uint8_t status = inb(PS2_STAT);
        if (!(status & 1)) continue;      /* nothing in the output buffer yet */
        uint8_t data = inb(PS2_DATA);
        if (status & 0x20) {              /* bit5: this byte is from the mouse */
            handle_mouse(data);
            continue;
        }
        int c = handle_key(data);
        if (c) return c;
    }
}

/* Non-blocking twin of input_getchar(): drains whatever's waiting in the
 * PS/2 buffer right now (mouse packets get applied along the way) and
 * returns the first key typed during that drain, or 0. The window manager
 * calls this every frame instead of blocking on a keypress. */
int input_poll(void)
{
    int first_key = 0;
    for (;;) {
        uint8_t status = inb(PS2_STAT);
        if (!(status & 1)) break;         /* buffer's empty, we're done */
        uint8_t data = inb(PS2_DATA);
        if (status & 0x20) {
            handle_mouse(data);
        } else {
            int c = handle_key(data);
            if (c && !first_key) first_key = c;
        }
    }
    return first_key;
}
