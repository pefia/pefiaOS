; boot/doom_wad.asm
;
; Pulls the shareware IWAD straight into the kernel image as read-only data.
; doom_app.c reads the bytes between these two symbols to answer PureDOOM's
; file I/O callbacks, so DOOM works without a disk or filesystem underneath it.
;
; incbin's path is resolved relative to nasm's cwd, which `make` runs from the
; project root, hence the bare filename.

section .rodata
align 4

global doom1_wad_start
global doom1_wad_end

doom1_wad_start:
    incbin "doom1.wad"
doom1_wad_end:
