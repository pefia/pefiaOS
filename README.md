# pefiaOS, press image below for showcase

[![Watch the showcase](https://img.youtube.com/vi/JEbIDeVyaeI/maxresdefault.jpg)](https://youtu.be/JEbIDeVyaeI)

A bare-metal **32-bit x86 operating system**, written from scratch in freestanding
C and NASM — no libc, no kernel dependencies. It boots via GRUB2 into protected
mode, brings up a VESA framebuffer, and runs a mouse-driven **graphical desktop**
with overlapping windows. On top of that it has a **from-scratch TCP/IP + TLS 1.3
network stack and web browser** that loads real HTTPS sites, **seven built-in
games**, and — yes — **the actual id Software DOOM**.

> Build: WSL2 Ubuntu + an `i686-elf` cross-compiler → `make` → a bootable
> `pefiaOS.iso` for QEMU or VirtualBox.

## Highlights

- **Boots to a graphical desktop** — window manager with z-order, drag-to-move,
  drag-to-resize, drop shadows, a Start menu with live search, and a taskbar clock.
- **From-scratch networking** — Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and TCP,
  driving Intel e1000 and Realtek RTL8139 NICs (auto-probed).
- **Hand-written TLS 1.3** — X25519, AES-128-GCM, SHA-256/HKDF — so HTTPS sites
  actually load.
- **A web browser** — HTTP/1.1 with redirects + chunked decoding, an HTML5
  renderer with links, lists, inline CSS colour, and inline images (BMP + JPEG
  decoded by hand).
- **Seven games** — Flappy Bird, Pong, a Mario-style platformer, a 3D raycaster
  maze, Tetris, Snake, and Breakout — all flicker-free and resizable.
- **It runs DOOM** — the real 1993 DOOM via [PureDOOM](https://github.com/Daivuk/PureDOOM),
  with the shareware WAD embedded right in the kernel image.

All of it is roughly **9k lines of kernel C** plus the boot stub (and the vendored
DOOM engine).

## Directory layout

```
pefiaOS/
├── Makefile                 # one-command build: `make`
├── linker.ld                # kernel memory layout (load at 1 MiB)
├── doom1.wad                # DOOM shareware IWAD (embedded into the image)
├── boot/
│   ├── boot.asm             # Multiboot v1 header (requests a framebuffer) + _start
│   └── doom_wad.asm         # incbin's doom1.wad into the kernel as read-only data
├── kernel/
│   ├── kernel.c             # kernel_main: brings everything up, hands off to the WM
│   ├── framebuffer.*, console.*, font8x16.h   # graphics + 8x16 text
│   ├── input.*, mouse.*     # polled PS/2 keyboard + mouse, software cursor
│   ├── heap.*, util.h, io.h # allocator + freestanding helpers + port I/O
│   ├── vfs.*, rtc.*, clock.* # in-RAM filesystem, CMOS time, rdtsc millisecond clock
│   ├── wm.*, taskbar.*      # window manager + desktop shell
│   ├── explorer.*, terminal.*, notepad.*, shell.*   # desktop apps
│   ├── pci.*, nic.*, e1000.*, rtl8139.*   # PCI + NIC drivers
│   ├── netstack.*, net.*    # Ethernet→TCP stack + HTTP(S) fetch
│   ├── crypto.*, tls.*      # SHA-256/HKDF/AES-GCM/X25519 + TLS 1.3 client
│   ├── browser.*, htmlrender.*, css.*, domparse.*, domrt.*, js.*   # the browser
│   ├── inflate.*, bitmap.*, jpeg.*, image.*   # DEFLATE + image decoders
│   ├── games.*              # Flappy Bird, Pong, Mario, Maze 3D, Tetris, Snake, Breakout
│   └── PureDOOM.h, puredoom.c, doom_app.*   # DOOM engine + pefiaOS platform layer
├── iso/boot/grub/grub.cfg   # GRUB menu (multiboot /boot/pefiaos.bin)
└── toolchain/
    ├── build-i686-elf.sh    # builds the i686-elf cross-compiler
    └── genfont.py           # regenerates kernel/font8x16.h from a PSF font
```

## Build (in WSL2 Ubuntu)

```sh
# 1. One-time: deps for building the cross-compiler, fonts, and the ISO
sudo apt update
sudo apt install -y build-essential bison flex libgmp3-dev libmpc-dev \
    libmpfr-dev texinfo wget nasm xorriso grub-pc-bin grub-common \
    qemu-system-x86 kbd netpbm

# 2. One-time: build the i686-elf cross-compiler (~20-40 min). Run from ~ .
bash toolchain/build-i686-elf.sh
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc

# 3. Build the OS
cd /mnt/c/Users/ish/Desktop/pefiaOS
make
```

This produces `pefiaOS.iso` in the project root. (`make clean` removes build
artifacts; the build embeds `doom1.wad`, so the kernel image is a few MB.)

## Run

- **QEMU (quick test):** `make run` — attaches a NAT-networked RTL8139 NIC so the
  browser can reach the real internet. `make run-net` also writes a `net.pcap`
  capture for debugging.
- **VirtualBox:** create a new VM (Type: Other, Version: Other/Unknown 32-bit),
  no hard disk, attach `pefiaOS.iso` as the optical disc, and give it **≥256 MB
  RAM** (DOOM wants room). Graphics Controller **VBoxVGA** (or VMSVGA) with VRAM
  ≥16 MB so the 1024×768×32 framebuffer is available. Click into the window to
  capture the mouse.

  **Networking** (Settings → Network → Adapter 1):
  - Enable Network Adapter, **Attached to: NAT**
  - **Advanced → Adapter Type: Intel PRO/1000 MT Desktop (82540EM)**
  - Cable Connected: checked

  VirtualBox does not emulate an RTL8139, so the Intel e1000 adapter type is
  required. pefiaOS auto-detects e1000 (VirtualBox) or RTL8139 (QEMU); PCnet and
  virtio-net are not yet supported.

## Using the desktop

- **Start menu** (bottom-left): launches every app and filters by typing.
- **Move a window** by dragging its title bar; **resize** by dragging the grip in
  its bottom-right corner; **close** with the red **X**; click any window to raise
  it. Open windows appear as buttons on the taskbar.

## Networking + Browser

Open **Browser** from the Start menu, type a URL (`https://` is assumed) and press
Enter. The stack is entirely from-scratch:

- **Drivers:** Intel e1000 + Realtek RTL8139, auto-probed (`nic.c`).
- **Stack:** Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP (`netstack.c`).
- **TLS 1.3:** X25519 + AES-128-GCM + SHA-256/HKDF, by hand (`crypto.c`, `tls.c`).
- **HTTP/1.1:** redirects + chunked transfer decoding (`net.c`).
- **Renderer:** word-wrapped HTML5 with headings, clickable links, lists, rules,
  entity decoding, inline CSS colour, inline images, and scrolling
  (`htmlrender.c`). JavaScript-heavy apps won't run, but static HTML/CSS,
  `<noscript>`, and simple `document.write` output render.

> Server certificates are **not** verified — pefiaOS reaches the web, it does not
> authenticate it. Appropriate for a hobby OS, not for secrets.

## Games

Seven games launch from the Start menu — each runs in a resizable window and
animates flicker-free via an off-screen buffer.

| Game        | Controls |
|-------------|----------|
| Flappy Bird | **Space / Up / W** to flap |
| Pong        | **Up/Down** or **W/S** (you vs. CPU) |
| Mario       | **←/→** or **A/D** to run, **Space/Up/W** to jump |
| Maze 3D     | **W/S** or **↑/↓** move, **A/D** or **←/→** turn (a from-scratch raycaster) |
| Tetris      | **←/→** move, **Up/W** rotate, **Down/S** soft drop, **Space** hard drop |
| Snake       | **Arrows** or **WASD** to steer, eat apples to grow |
| Breakout    | **←/→** or **A/D** to move the paddle, **Space** to launch |

After a game over, press **Space** to restart.

## Yes, it runs DOOM

The Start menu's **DOOM** entry runs **the actual id Software DOOM (1993)** — not a
clone — via [PureDOOM](https://github.com/Daivuk/PureDOOM), a zero-dependency port
of the original source. The freely-distributable shareware `doom1.wad` is embedded
directly in the kernel image, so no disk or filesystem is needed. pefiaOS supplies
DOOM's platform layer: memory → `kmalloc`, timing → `clock_ms`, a RAM-backed file
layer over the embedded WAD, raw key up/down events, and a scancode → DOOM key
mapping. DOOM's 320×200 framebuffer is scaled into the window each frame.

Controls are the classics: **arrows** move/turn, **Ctrl** fire, **Space** use/open
doors, **Alt** strafe, **Shift** run, number keys select weapons, **Esc** menu,
**Enter** select.

> No sound yet (there's no audio driver); savegames live in RAM for the session;
> and since the engine keeps global state, DOOM is happiest opened once per boot.

## Shell commands

In the **Terminal** app: `help`, `about` (neofetch-style system info), `clear`,
`echo TEXT`, `memtest` (heap stress test).

## Regenerating the font

`kernel/font8x16.h` is generated from a Linux console PSF font:

```sh
python3 toolchain/genfont.py     # writes kernel/font8x16.h
```

## Limitations (by design)

- TLS doesn't verify certificates; the crypto isn't hardened or constant-time.
- JavaScript isn't really executed (only trivial `document.write`).
- No preemptive multitasking — the main loop is cooperative and busy-polls.
- No sound, and no PIT/APIC timer (timing rides on `rdtsc`).

Each is a deliberate scope choice — and a clear next project.

## License & credits

pefiaOS is licensed under the **GNU GPL v2** (see [LICENSE](LICENSE)). It must be
GPLv2 because it links the DOOM engine: **DOOM** is provided by
[PureDOOM](https://github.com/Daivuk/PureDOOM), which is GPLv2 (derived from id
Software's GPL-released DOOM source). The embedded [`doom1.wad`](doom1.wad) is the
**official DOOM shareware** episode, which id Software permits to be freely
redistributed; DOOM © id Software.

The pefiaOS kernel, drivers, network stack, TLS, browser, and games are original
work. AI has been used to comment and reformat the code for easy readability + efficiency, and debugging Full attribution is in [CREDITS.md](CREDITS.md).
