# Devlog — Bringing Games (and a 3D Engine) to pefiaOS

This entry covers the work that turned pefiaOS from "a desktop that browses the
web" into "a desktop you can actually *play* on": five recognisable games, a new
flicker-free rendering path, real-time keyboard input, and resizable windows.

The goal was a demo that *proves the OS works well* — not tech-demo stubs, but
games anyone recognises in a second: **Flappy Bird, Pong, a Mario-style level, a
Wolfenstein-style 3D maze, and Tetris.**

---

## The problem: the OS wasn't built for animation

Everything in pefiaOS up to this point was **event-driven**. The window manager
only repainted the screen when you pressed a key or moved the mouse, and it
painted by writing straight to the live framebuffer. That's perfect for a text
editor and totally wrong for a game, which needs to redraw 30 times a second
whether or not you touch anything — and needs to do it without flickering.

There were three concrete gaps, and I fixed each before writing a single game.

### 1. Input had no concept of a "held" key

`input.c` only exposed `input_poll()`, which returns a *typed character* — the
same thing a text box wants. It had two fatal problems for games:

- **No held-key state.** You can't ask "is the player holding Right?" — you only
  find out when the PS/2 controller's auto-repeat decides to send another
  character, which lags and stutters.
- **Arrow keys were silently dropped.** They arrive as `0xE0`-prefixed extended
  scan codes, which the old keymap mapped to nothing.

I extended `input.c` **additively** — the text path is untouched — with a
real-time key-state layer:

```c
int input_key_down(int code);     /* is it held right now?            */
int input_key_pressed(int code);  /* did it go down since I last asked? (latched) */
```

`handle_key()` now also maintains a `keydown[]` bitmap and a `keylatch[]` of
edges. The latch is the important bit: a quick tap sets the latch on key-down and
it *stays* set until a game reads it, so even a flap pressed-and-released between
two frames is never missed. Arrow keys are decoded from the `0xE0` prefix and
reported only through this API. Games use `_down` for movement (run, turn, paddle)
and `_pressed` for actions (flap, jump, rotate, hard-drop).

### 2. Drawing straight to the screen flickers

Animating means clearing and redrawing every frame, and doing that on the live
framebuffer shows half-drawn frames. The fix is the classic one: **compose
off-screen, then present in one pass.** I added a single primitive to the
framebuffer driver:

```c
void fb_blit(int x, int y, int w, int h, const color_t *src);
```

It copies a finished `w*h` block of pixels onto the screen (row-at-a-time memcpy
on 32-bpp, clipped to the framebuffer). Each game owns an off-screen buffer the
size of its window's content area, paints the whole frame into it, and blits once.
No tearing, no flicker.

### 3. The main loop never ticked anything on its own

The WM loop only did work in response to input. I added a per-iteration tick for
the **focused** game window:

```c
if (top window is a game) game_tick(g, content_rect);
```

`game_tick()` self-paces against `clock_ms()` (the existing rdtsc-based clock the
network stack uses) — so each game advances at a fixed timestep (Pong 16 ms,
Mario 22 ms, Flappy 28 ms, Tetris/Maze 33 ms) no matter how fast the loop spins.
Only the focused game animates; background windows stay frozen, which keeps it
cheap.

---

## One interface, five games

Rather than five window types, all games sit behind a single `WIN_GAME` kind and
a uniform interface in `games.h` (`game_new/_paint/_tick/_key/_resize/_free`).
The WM gained exactly one branch. State lives in one `Game` struct; the off-screen
buffer is freed through a `game_free` hook wired into the WM's window-close path
so nothing leaks.

A couple of shared building blocks were needed because this is freestanding C
with no libm:

- **PRNG** — a tiny xorshift seeded from `rdtsc` (pipes, ball serves, tetromino
  bag).
- **Sine/cosine** — a 360-entry integer table built at startup with **Bhaskara
  I's** degree-form sine approximation (accurate to a fraction of a percent, no
  floating point), scaled by 1024. This is what makes the 3D maze possible.

### Flappy Bird
Fixed-point vertical physics (gravity + flap impulse), three recycling pipes with
randomised gaps, AABB collision, persistent best score. All distances scale to the
window so it plays the same at any size.

### Pong
You're the left paddle (Up/Down or W/S); the CPU tracks the ball with a capped
speed so it's beatable. Ball angle changes based on where it hits the paddle.
Endless, first-to-nothing — just rally and rack up points.

### Mario-style level
The most involved one. A tile map (ground with pits, floating `?`-blocks and brick
platforms, coins, a goal flag) with **pixel-stepped AABB collision** resolved one
axis at a time, so no tunnelling. Run + jump physics, a scrolling camera, three
patrolling enemies you can **stomp** (or die to), coins, lives, and a win state at
the flag.

### Maze 3D — the showpiece
A **raycaster** in the Wolfenstein tradition: for each screen column it marches a
ray through a 16×16 grid using the integer sin/cos table, finds the wall, applies
**fish-eye correction**, and draws a vertical wall strip whose height is inversely
proportional to distance. Walls are **distance-shaded** (fog) and N/S faces are
drawn darker than E/W for depth. Three wall colours, ceiling/floor fills, smooth
WASD/arrow movement with wall collision. This is the one that proves the OS can do
real-time 3D from scratch.

### Tetris
All seven tetrominoes encoded as 16-bit 4×4 rotation masks, full collision and
rotation (with basic wall-kicks), line clears with cascading shift-down, gravity
that speeds up with level, soft drop (Down) and hard drop (Space), scoring, and a
game-over/restart loop.

---

## Bonus: resizable windows

Games made the lack of window resizing obvious, so the WM now has a **resize grip**
in every window's bottom-right corner (three diagonal ticks). Drag it to resize,
clamped to a sensible minimum and to the screen bounds. Existing apps lay out from
their content rectangle, so they reflow for free; game windows reallocate their
off-screen buffer on the fly and keep playing.

---

## How to play

Open the **Start menu** and pick *Flappy Bird*, *Pong*, *Mario*, *Maze 3D*, or
*Tetris*.

| Game     | Controls |
|----------|----------|
| Flappy   | **Space / Up / W** to flap |
| Pong     | **Up/Down** or **W/S** |
| Mario    | **←/→** or **A/D** to run, **Space/Up/W** to jump |
| Maze 3D  | **W/S** or **↑/↓** to move, **A/D** or **←/→** to turn |
| Tetris   | **←/→** move, **Up/W** rotate, **Down/S** soft drop, **Space** hard drop |

After a game over, press **Space** to restart.

---

## Boss level: yes, it runs DOOM

Five home-grown games is a nice showcase, but there's only one true rite of
passage for an OS — **can it run DOOM?** pefiaOS now does, and it's the *actual*
id Software DOOM (1993), not a look-alike.

### Why this was even possible
DOOM expects a libc and a host operating system. pefiaOS has neither. The bridge
is **[PureDOOM](https://github.com/Daivuk/PureDOOM)** — a zero-dependency port of
the original DOOM source that compiles as a single file and asks the host for
everything through callbacks. I verified up front that it compiles cleanly with
our freestanding `i686-elf` toolchain and links with **zero undefined symbols**
(it ships its own `doom_memcpy`/`doom_strlen`/etc.), which is exactly what a
no-libc kernel needs.

### What pefiaOS had to provide
PureDOOM is the engine; pefiaOS is the "operating system" underneath it. I wrote a
platform layer (`doom_app.c`) that hands DOOM:

- **Memory** → `kmalloc`/`kfree` (DOOM's zone allocator sits on top of our heap).
- **Time** → `clock_ms()`, reported as seconds/microseconds. DOOM uses this to run
  its fixed 35 Hz tic clock; `doom_update()` runs exactly as many tics as real
  time has elapsed, so the game runs at the right speed.
- **Files** → there is no disk, so I built a tiny **RAM filesystem**. Reads of
  `doom1.wad` are served straight from the IWAD bytes **embedded in the kernel
  image** (`boot/doom_wad.asm` `incbin`s the 4 MB shareware WAD into `.rodata`);
  config and savegames live in RAM scratch files so the engine doesn't choke.
  DOOM's own version-detection loop opens each known WAD in turn — we answer "not
  found" for everything except `doom1.wad`, which lands it in shareware mode.
- **Input** → DOOM needs key-*down and key-up* for many keys (Ctrl, Alt, arrows,
  Esc…), but the desktop only ever needed typed characters. So I added a raw
  **key-event queue** to `input.c`: every make/break the keyboard produces is
  queued with its scancode and an extended-key flag. Each frame the DOOM app
  drains the queue and maps pefiaOS scancodes → DOOM keycodes.
- **Display** → DOOM renders to a 320×200 8-bit-palette framebuffer; PureDOOM
  hands it back as RGBA. Each frame I convert and **nearest-neighbour scale** it
  into the window with `fb_blit`, so DOOM is just another resizable pefiaOS window
  sitting on the desktop next to the browser.

### Keeping it from taking the whole OS down
If DOOM hits a fatal `I_Error`, it calls `exit()`. We can't exit a process — there
are none. So the exit callback does a `__builtin_longjmp` back to a
`__builtin_setjmp` wrapped around the per-frame update; a DOOM failure becomes a
"DOOM stopped" box in the window instead of a dead machine. I also bumped the
kernel stack from 16 KB to **128 KB**, because DOOM's recursive BSP traversal and
fat render locals need far more room than anything else in the OS.

### It actually works
Booted headless in QEMU and screenshotted the framebuffer: DOOM loads the WAD,
renders its 3D world with the correct palette, draws the full status bar
(ammo/health/face/armor), runs the attract-mode demo, and animates in real time —
all inside a window on the pefiaOS desktop. Controls are the originals: arrows to
move/turn, Ctrl to fire, Space to open doors, Alt to strafe, Shift to run, number
keys for weapons, Esc/Enter for the menu. (No sound — there's no audio device
yet.)

*The honest caveats:* there's no audio; savegames live only in RAM for the
session; and since PureDOOM keeps global state, DOOM is happiest opened once per
boot.

## What I'd reach for next

- A real timer interrupt (PIT) instead of busy-waiting the clock, so games — and
  DOOM — don't peg the CPU.
- Sound: a Sound Blaster / AC'97 driver would give DOOM its music and SFX (the PC
  speaker is enough for the home-grown games' blips).
- A global double-buffer for the *whole* desktop, reusing `fb_blit`, so dragging
  windows is as smooth as the games and DOOM now are.
