/* kernel/input.c */
#include "input.h"
#include "io.h"
#include "mouse.h"

#include <stdint.h>

#define DATA 0x60
#define STAT 0x64

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

/* US QWERTY, scan code set 1. 0 = no printable character. */
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

/* --- PS/2 controller plumbing --- */

static void wait_write(void) { int t = 100000; while ((inb(STAT) & 2) && t--) { } }
static void wait_read(void)  { int t = 100000; while (!(inb(STAT) & 1) && t--) { } }

static void mouse_command(uint8_t cmd)
{
    wait_write(); outb(STAT, 0xD4);   /* address next byte to the mouse */
    wait_write(); outb(DATA, cmd);
    wait_read();  (void)inb(DATA);    /* consume the 0xFA ACK */
}

void input_init(void)
{
    /* Drain anything pending. */
    while (inb(STAT) & 1) (void)inb(DATA);

    wait_write(); outb(STAT, 0xA8);   /* enable the auxiliary (mouse) port */

    wait_write(); outb(STAT, 0x20);   /* read the controller config byte */
    wait_read();  uint8_t cfg = inb(DATA);
    cfg |=  0x02;                      /* enable IRQ12 (harmless when polling) */
    cfg &= ~0x20;                      /* clear "mouse clock disabled" bit */
    wait_write(); outb(STAT, 0x60);   /* write config byte back */
    wait_write(); outb(DATA, cfg);

    mouse_command(0xF6);              /* set defaults */
    mouse_command(0xF4);              /* enable data reporting */
}

/* --- input handling --- */

/* --- real-time key state (for games); see input.h --- */
static uint8_t keydown[IK_COUNT];
static uint8_t keylatch[IK_COUNT];

/* Map a (possibly 0xE0-extended) scan code to an IK_* slot, or -1. */
static int sc_state_index(uint8_t code, int ext)
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
    return (code >= 0 && code < IK_COUNT) ? keydown[code] : 0;
}

int input_key_pressed(int code)
{
    if (code < 0 || code >= IK_COUNT) return 0;
    int v = keylatch[code];
    keylatch[code] = 0;
    return v;
}

/* --- raw key-event ring (for DOOM); see input.h --- */
#define EVQ_SIZE 128
static uint8_t evq_code[EVQ_SIZE], evq_press[EVQ_SIZE], evq_ext[EVQ_SIZE];
static int     evq_head, evq_tail;

static void evq_push(uint8_t code, uint8_t pressed, uint8_t ext)
{
    int n = (evq_head + 1) % EVQ_SIZE;
    if (n == evq_tail) return;        /* full: drop oldest-unread by ignoring */
    evq_code[evq_head]  = code;
    evq_press[evq_head] = pressed;
    evq_ext[evq_head]   = ext;
    evq_head = n;
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
    static int shift = 0;
    static int ext   = 0;

    if (sc == 0xE0) { ext = 1; return 0; }    /* extended-key prefix */

    int     release = sc & 0x80;
    uint8_t code    = sc & 0x7F;
    int     was_ext = ext;
    ext = 0;

    /* Queue the raw event for any consumer that wants down+up for every key. */
    evq_push(code, release ? 0 : 1, (uint8_t)was_ext);

    /* Update real-time state. Typematic repeat resends the make code without a
     * break, so latch only on the first make (state was previously up). */
    int idx = sc_state_index(code, was_ext);
    if (idx >= 0) {
        if (release) keydown[idx] = 0;
        else { if (!keydown[idx]) keylatch[idx] = 1; keydown[idx] = 1; }
    }

    if (release) {
        if (code == SC_LSHIFT || code == SC_RSHIFT) shift = 0;
        return 0;
    }
    if (code == SC_LSHIFT || code == SC_RSHIFT) { shift = 1; return 0; }
    if (was_ext) return 0;            /* extended (arrow) keys: state API only */

    char c = shift ? keymap_shift[code] : keymap[code];
    return c ? (int)(unsigned char)c : 0;
}

static void handle_mouse(uint8_t byte)
{
    static int phase = 0;
    static int pkt[3];

    switch (phase) {
    case 0:
        if (!(byte & 0x08)) return;   /* bit3 always set on byte 0: resync */
        pkt[0] = byte; phase = 1;
        break;
    case 1:
        pkt[1] = byte; phase = 2;
        break;
    default: {
        pkt[2] = byte; phase = 0;
        int flags = pkt[0];
        if (flags & 0xC0) break;      /* X/Y overflow: drop this packet */
        int dx = pkt[1] - ((flags << 4) & 0x100);
        int dy = pkt[2] - ((flags << 3) & 0x100);
        mouse_set_buttons((unsigned)(flags & 0x07));
        mouse_move(dx, -dy);          /* screen Y grows downward */
        break;
    }
    }
}

int input_getchar(void)
{
    for (;;) {
        uint8_t st = inb(STAT);
        if (!(st & 1)) continue;      /* output buffer empty */
        uint8_t data = inb(DATA);
        if (st & 0x20) {              /* bit5 set: byte came from the mouse */
            handle_mouse(data);
            continue;
        }
        int c = handle_key(data);
        if (c) return c;
    }
}

/* Non-blocking sibling of input_getchar: drains every byte currently waiting in
 * the PS/2 buffer (updating the mouse along the way) and returns the first key
 * character seen this call, or 0 if none. The window manager calls this every
 * loop iteration instead of blocking. */
int input_poll(void)
{
    int key = 0;
    for (;;) {
        uint8_t st = inb(STAT);
        if (!(st & 1)) break;         /* nothing left to read */
        uint8_t data = inb(DATA);
        if (st & 0x20) {
            handle_mouse(data);
        } else {
            int c = handle_key(data);
            if (c && !key) key = c;   /* keep the first key of this batch */
        }
    }
    return key;
}
