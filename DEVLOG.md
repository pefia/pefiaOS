# pefiaOS — Full Project Devlog (in build order)

This is the story of pefiaOS from the first line of assembly to the latest
feature, in roughly the order I actually built things. Each stage only became
possible because the one before it worked, so reading top-to-bottom is also a
decent map of how a graphical, internet-connected OS is assembled from nothing.

pefiaOS is a **bare-metal 32-bit x86 operating system** with a graphical
desktop, a from-scratch TCP/IP + TLS 1.3 network stack, a web browser, and a set
of games — all freestanding C and NASM, no libc, no third-party code in the
kernel.

> Build: WSL2 Ubuntu + an `i686-elf` cross-compiler → `make` → `pefiaOS.iso`.
> Runs in QEMU and VirtualBox.

---

## 1. Boot: getting the CPU to run my code
**`boot/boot.asm`, `linker.ld`**

The very first thing was a **Multiboot v1 header** in NASM so GRUB would load the
kernel. The header also *requests a graphics mode* — a 1024×768×32 linear
framebuffer — so that by the time my code runs, there's already a pixel buffer to
draw into (no messing with VGA text mode or mode-setting from the kernel). The
linker script (`linker.ld`) lays the kernel out at the 1 MiB physical mark and
exports `_kernel_end`, which later becomes the start of the heap. `_start` sets up
a stack and calls `kernel_main`.

## 2. Pixels on screen
**`kernel/framebuffer.c/.h`, `kernel/multiboot.h`**

The framebuffer driver reads the framebuffer address, pitch, and pixel format
from the Multiboot info GRUB hands over, and exposes the primitives everything
else is built on: `fb_put_pixel`, `fb_fill_rect`, `fb_rgb` (which handles
RGB-vs-BGR channel ordering), and `fb_scroll_up`. GRUB's emulated VBE sometimes
reports bogus channel masks, so there's a fallback to standard layouts per depth.
This was the "hello world" moment — filling the screen with a colour.

## 3. Text and a font
**`kernel/console.c/.h`, `kernel/font8x16.h`, `toolchain/genfont.py`**

To print anything I needed glyphs, so I generated an 8×16 bitmap font from a Linux
console PSF (`genfont.py` → `font8x16.h`) and wrote a scrolling text console on
top of the framebuffer (`gfx_text` for raw strings, plus a full cell-based console
with newline/scroll/tab/backspace).

## 4. Freestanding C foundations
**`kernel/util.h`, `kernel/io.h`**

No libc means writing my own: `kmemset`, `kmemmove`, `kstrlen`, `kstrcpy`,
`kstrcat`, `kutoa` (header-only), plus `inb`/`outb` port I/O helpers. Small, but
everything downstream leans on them.

## 5. Input: keyboard + mouse
**`kernel/input.c/.h`, `kernel/mouse.c/.h`**

A single **polled PS/2 driver** services both devices off one status register.
Scan-code-set-1 keymaps (with Shift) turn key presses into characters; the mouse
packets move a **software cursor with save-under** (it stashes the pixels behind
the cursor and restores them when it moves, so the cursor floats over anything).
Polling rather than interrupts kept the whole system simple and predictable.

## 6. Memory: a real heap
**`kernel/heap.c/.h`**

A **first-fit free-list allocator** placed in the largest available RAM region
(discovered from the Multiboot memory map, kept clear of the kernel). `kmalloc`
splits blocks; `kfree` coalesces neighbours to fight fragmentation. This unlocked
dynamically allocating windows, app state, network buffers — everything after
this point.

## 7. A filesystem and a clock
**`kernel/vfs.c/.h`, `kernel/rtc.c/.h`, `kernel/clock.c/.h`**

An in-memory **VFS** (directories + files) gives the Explorer something to show.
`rtc.c` reads wall-clock time from the CMOS for the taskbar clock. `clock.c` is a
millisecond time source built on **`rdtsc` calibrated against the RTC** — there's
no PIT/APIC timer wired up, so this single clock later drives both network
timeouts and game timestep pacing.

## 8. The desktop: window manager + apps
**`kernel/wm.c/.h`, `kernel/taskbar.c`, `kernel/explorer.c`, `kernel/terminal.c`, `kernel/notepad.c`, `kernel/shell.c`**

This is where it started to feel like an OS. The **window manager** keeps windows
in z-order, composites a gradient desktop + drop-shadowed windows + a taskbar each
repaint, and routes input: keys to the focused window, clicks to
raise/drag/close or into the app. The **taskbar** has a Start menu with a search
box that filters apps and files. The first apps:

- **File Explorer** over the VFS,
- **Terminal** (with a small shell: `help`, `about` neofetch screen, `clear`,
  `echo`, `memtest`),
- **Notepad** (a text buffer with a cursor).

## 9. Talking to hardware: PCI + NICs
**`kernel/pci.c/.h`, `kernel/rtl8139.c`, `kernel/e1000.c`, `kernel/nic.c`**

To get online I first had to find a network card. `pci.c` enumerates the PCI bus;
`nic.c` auto-probes and drives whichever NIC is present: **Realtek RTL8139** (what
QEMU emulates) or **Intel e1000 / 82540EM** (what VirtualBox provides). Each driver
sets up receive/transmit rings and raw Ethernet frame send/receive.

## 10. The network stack, from Ethernet up
**`kernel/netstack.c`, `kernel/net.c/.h`**

On top of raw frames I built the whole stack by hand: **Ethernet → ARP → IPv4 →
ICMP/UDP/TCP**, a **DHCP** client for auto-configuration (IP, gateway, DNS), a
**DNS** resolver, and a **TCP** state machine with retransmission timing (paced by
`clock.c`). `net.c` sits on top as the HTTP/1.1 client: redirects and chunked
transfer decoding.

## 11. Cryptography and TLS 1.3 (the hard part)
**`kernel/crypto.c/.h`, `kernel/tls.c/.h`**

HTTPS meant implementing crypto from scratch: **SHA-256, HMAC, HKDF, AES-128-GCM,
and X25519**, then a **TLS 1.3 client** handshake (key share, HKDF-Expand-Label
key schedule, AEAD record protection) on top of them. The honest caveat, stated in
the headers: it's not constant-time and **doesn't verify certificates** — it
reaches the web, it doesn't authenticate it. But it's enough that real HTTPS sites
load.

## 12. The web browser + HTML renderer
**`kernel/browser.c`, `kernel/htmlrender.c`, `kernel/css.c`, `kernel/domparse.c`, `kernel/domrt.c`, `kernel/js.c`**

The browser ties it together: type a URL, fetch over HTTP(S), and render. The
renderer does word-wrapped HTML5 with headings, links (clickable), lists, rules,
entity decoding, inline CSS colour, and scrolling. There's a minimal DOM parser
and runtime scaffold and a JavaScript *degradation* path (it surfaces `<noscript>`
and simple `document.write` output) — full JS is explicitly out of scope.

## 13. Images: decoding formats by hand
**`kernel/inflate.c`, `kernel/bitmap.c`, `kernel/jpeg.c`, `kernel/image.c`**

To render `<img>`, I added a DEFLATE (`inflate.c`), a BMP decoder (24/32-bit), and
a JPEG decoder (Huffman + DCT). `image.c` dispatches by format, and the renderer
fetches and draws images inline via a bounded `net_fetch_limited()` so a page
can't pull down unbounded data.

## 14. Games + a real-time rendering path *(latest)*
**`kernel/games.c/.h`, plus changes to `input.c`, `framebuffer.c`, `wm.c`**

The newest work made pefiaOS *playable* with five recognisable games — **Flappy
Bird, Pong, a Mario-style level, a Wolfenstein-style 3D maze (raycaster), and
Tetris** — and required teaching the OS to animate:

1. **Real-time input** — added `input_key_down` / `input_key_pressed` (held-state
   + latched edges) alongside the existing typed-char path, and decoded the arrow
   keys (`0xE0` extended scan codes) that were previously dropped.
2. **Flicker-free rendering** — added `fb_blit()` so each game composes a frame in
   an off-screen buffer and presents it in one pass.
3. **A game loop** — the WM now ticks the focused game every iteration, self-paced
   against `clock.c` for a fixed timestep.
4. **A 3D engine** — a raycaster using an integer sine table (Bhaskara I's
   approximation, no libm) for the Maze 3D showpiece.
5. **Resizable windows** — a drag grip in every window's bottom-right corner.

(Full detail in **DEVLOG-GAMES.md**.)

## 15. Yes, it runs DOOM *(latest)*
**`kernel/puredoom.c`, `kernel/doom_app.c`, `boot/doom_wad.asm`, `kernel/PureDOOM.h`**

The final milestone is the OS rite of passage: running the **actual id Software
DOOM (1993)**. The engine is [PureDOOM](https://github.com/Daivuk/PureDOOM), a
zero-dependency port that compiles against our freestanding toolchain with no libc
and no undefined symbols. pefiaOS supplies DOOM's platform layer:

- memory via `kmalloc`, time via `clock.c`;
- a small **RAM filesystem** whose only real file is the shareware `doom1.wad`,
  **embedded in the kernel image** by `incbin` (no disk needed);
- a raw **key-event queue** added to `input.c` (DOOM needs key up/down for many
  keys, which the desktop never did);
- per-frame conversion + scaling of DOOM's 320×200 framebuffer into a normal
  resizable window.

A `setjmp`/`longjmp` guard turns a DOOM `I_Error` into a message box instead of a
kernel death, and the boot stack was raised to 128 KB for DOOM's deep recursion.
Verified by booting headless and screenshotting DOOM rendering, HUD and all, on
the desktop. (Full detail in **DEVLOG-GAMES.md**.)

---

## The shape of it

```
boot.asm ─► framebuffer ─► console/font ─► input/mouse ─► heap
   ─► vfs/rtc/clock ─► window manager ─► explorer/terminal/notepad
   ─► pci ─► nic (rtl8139/e1000) ─► netstack (eth/arp/ip/tcp/dhcp/dns)
   ─► crypto ─► tls ─► http/net ─► browser/html/css/dom ─► images
   ─► games + real-time rendering ─► DOOM
```

~9k lines of kernel C (plus the embedded DOOM engine and the boot stub). Every
layer is something you usually take for granted — a malloc, a TCP handshake, a TLS
key schedule, a sine — written out by hand. The reward is that you can boot
`pefiaOS.iso`, open the browser to a real HTTPS site, go play Tetris, and then
fire up **DOOM**.

## Honest limitations
- TLS does not verify certificates; crypto isn't hardened or constant-time.
- JavaScript is not really executed (only trivial `document.write`).
- No preemptive multitasking; the main loop is cooperative and busy-polls.
- No sound, and no PIT/APIC timer (timing rides on `rdtsc`).

These are deliberate scope choices for a hobby OS, and each one is a clear next
project.

---

*Built by Ish — see README.md to build and run, DEVLOG-GAMES.md for the games
deep-dive.*
