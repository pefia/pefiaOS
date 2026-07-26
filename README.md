# pefiaOS

[![Watch the showcase](https://img.youtube.com/vi/JEbIDeVyaeI/maxresdefault.jpg)](https://youtu.be/JEbIDeVyaeI)

A 32-bit x86 operating system written completely from scratch in freestanding C and NASM. It boots through GRUB2, enters protected mode, initializes a VESA framebuffer, and launches a graphical desktop with windows, networking, a web browser, games, and even the original DOOM.

Everything apart from the bundled DOOM engine was written specifically for this project, including the networking stack, TLS implementation, browser, window manager, drivers, and desktop applications.

> Built in WSL2 Ubuntu using an `i686-elf` cross compiler. `make` produces a bootable ISO that runs in QEMU or VirtualBox.

## Features

### Desktop

- Window manager with overlapping windows
- Drag, resize, minimize and close windows
- Taskbar with clock
- Start menu with live search
- Mouse support
- Software rendered desktop with shadows

### Networking

The networking stack is written from scratch and includes:

- Ethernet
- ARP
- IPv4
- ICMP
- UDP
- DHCP
- DNS
- TCP

Supports both Intel e1000 and Realtek RTL8139 network cards.

### HTTPS

pefiaOS includes its own TLS 1.3 client implementation supporting:

- X25519 key exchange
- HKDF
- SHA-256
- AES-128-GCM

This allows the browser to load real HTTPS websites without relying on external libraries.

### Browser

The browser supports:

- HTTP/1.1
- HTTPS
- Redirects
- Chunked transfer encoding
- HTML rendering
- Headings
- Lists
- Links
- Basic inline CSS colours
- BMP and JPEG images
- Scrolling

It is intentionally simple and does not aim to support modern JavaScript-heavy websites.

### Games

Built-in games include:

- Flappy Bird
- Pong
- Mario-style platformer
- Maze 3D raycaster
- Tetris
- Snake
- Breakout

All games run inside resizable desktop windows.

### DOOM

pefiaOS also runs the original 1993 DOOM through PureDOOM.

The shareware WAD is embedded directly into the kernel image, so no filesystem access is required. The operating system provides memory allocation, timing, keyboard input and file access required by the engine.

## Project structure

```
pefiaOS/
├── boot/
├── kernel/
├── iso/
├── toolchain/
├── Makefile
├── linker.ld
└── doom1.wad
```

Most kernel code lives inside `kernel/`.

Some major components include:

| Component | Description |
|----------|-------------|
| framebuffer | Graphics output |
| wm | Window manager |
| taskbar | Desktop shell |
| browser | Web browser |
| htmlrender | HTML renderer |
| tls | TLS 1.3 implementation |
| crypto | Cryptographic primitives |
| netstack | TCP/IP networking |
| e1000 / rtl8139 | Network drivers |
| terminal | Shell |
| notepad | Text editor |
| explorer | File explorer |
| games | Built-in games |

## Building

Install the required packages:

```bash
sudo apt update

sudo apt install -y \
build-essential \
bison \
flex \
libgmp3-dev \
libmpc-dev \
libmpfr-dev \
texinfo \
wget \
nasm \
xorriso \
grub-pc-bin \
grub-common \
qemu-system-x86 \
kbd \
netpbm
```

Build the cross compiler:

```bash
bash toolchain/build-i686-elf.sh
```

Add it to your PATH:

```bash
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

Build the operating system:

```bash
make
```

This creates `pefiaOS.iso`.

## Running

Quickest option:

```bash
make run
```

This launches QEMU with NAT networking enabled.

VirtualBox is also supported using an Intel PRO/1000 (82540EM) adapter.

## Desktop applications

- Browser
- File Explorer
- Terminal
- Notepad
- DOOM
- Seven built-in games

## Shell commands

The terminal currently includes commands such as:

- help
- about
- clear
- echo
- memtest

## Limitations

This is still a hobby operating system, so several things are intentionally unfinished.

- No virtual memory or paging
- Cooperative multitasking
- TLS certificates are not verified
- Very limited JavaScript support
- No USB support
- In-memory filesystem

These are all areas I'd like to improve in future versions.

## Statistics

Approximately:

- ~9,000 lines of kernel C
- Freestanding C and NASM
- 32-bit x86
- Boots through GRUB2
- Runs entirely without libc or an existing kernel

## Credits

pefiaOS is licensed under **GPL v2** because it links against the GPL-licensed PureDOOM engine.

The operating system itself, including the kernel, networking stack, browser, drivers, desktop environment and applications, was written by me.

AI was used for debugging, explaining code, and occasionally restructuring code. Full attribution is available in `CREDITS.md`.
