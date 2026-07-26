section .rodata
align 4

global doom1_wad_start
global doom1_wad_end

doom1_wad_start:
    incbin "doom1.wad"
doom1_wad_end:
