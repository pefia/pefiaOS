; boot/doom_wad.asm
; -----------------------------------------------------------------------------
; Embeds the freely-distributable DOOM shareware IWAD (doom1.wad) directly into
; the kernel image as read-only data, exposing start/end symbols the C side
; (doom_app.c) hands to PureDOOM through its file-I/O callbacks. This avoids
; needing a real disk or filesystem: the WAD is just bytes in memory.
;
; incbin is resolved relative to nasm's working directory, which is the project
; root when `make` runs.
; -----------------------------------------------------------------------------

section .rodata
global doom1_wad_start
global doom1_wad_end

align 4
doom1_wad_start:
    incbin "doom1.wad"
doom1_wad_end:
