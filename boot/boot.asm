; boot/boot.asm
;
; Multiboot v1 entry stub. GRUB2 drops us here already in 32-bit protected
; mode with paging off, so there's no real/protected mode transition to do
; ourselves - just enough setup to call into C.
;
; We ask for a linear framebuffer through the Multiboot video fields (mode 0)
; instead of relying on VGA text mode, since the desktop needs pixel access.

MB_MAGIC    equ 0x1BADB002
MB_ALIGN    equ 1 << 0
MB_MEMINFO  equ 1 << 1
MB_VIDEO    equ 1 << 2
MB_FLAGS    equ MB_ALIGN | MB_MEMINFO | MB_VIDEO
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

FB_MODE     equ 0      ; linear graphics framebuffer (1 would mean text mode)
FB_WIDTH    equ 1024
FB_HEIGHT   equ 768
FB_DEPTH    equ 32

STACK_SIZE  equ 128 * 1024   ; DOOM's renderer + BSP walk recurse deep enough
                              ; that the old 16 KiB stack wasn't enough headroom

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    ; a.out-kludge header address fields - unused since we're a plain ELF
    ; kernel, but they still have to occupy these 5 dwords or the video mode
    ; fields below land at the wrong offset and GRUB won't see them.
    dd 0
    dd 0
    dd 0
    dd 0
    dd 0
    dd FB_MODE
    dd FB_WIDTH
    dd FB_HEIGHT
    dd FB_DEPTH

section .bss
align 16
kstack:
    resb STACK_SIZE
kstack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, kstack_top
    mov ebp, 0          ; zero frame pointer so a stack walker knows to stop here
    cld                 ; GRUB doesn't guarantee DF=0; our string ops assume forward

    ; GRUB hands us the Multiboot magic in eax and the info struct in ebx.
    ; cdecl pushes right-to-left, so this calls kernel_main(magic, mb_info).
    push ebx
    push eax
    call kernel_main

    ; kernel_main is not supposed to return, but if it ever does, stop cleanly
    ; instead of running off into whatever memory follows.
.halt:
    cli
    hlt
    jmp .halt
