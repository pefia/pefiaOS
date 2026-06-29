; boot/boot.asm
; -----------------------------------------------------------------------------
; pefiaOS boot stub (NASM).
;
; GRUB2 enters here in 32-bit protected mode (the Multiboot contract). This
; version asks GRUB for a LINEAR GRAPHICS framebuffer via the Multiboot v1
; video fields, then passes the Multiboot magic + info pointer to the C kernel
; so it can locate the framebuffer.
; -----------------------------------------------------------------------------

; ---- Multiboot v1 header flags ----
MBALIGN  equ 1 << 0              ; align loaded modules on page boundaries
MEMINFO  equ 1 << 1             ; provide a memory map
VIDMODE  equ 1 << 2             ; request a video mode (framebuffer)
MBFLAGS  equ MBALIGN | MEMINFO | VIDMODE
MAGIC    equ 0x1BADB002          ; magic that lets GRUB find the header
CHECKSUM equ -(MAGIC + MBFLAGS)  ; magic + flags + checksum must equal 0

section .multiboot
align 4
    dd MAGIC
    dd MBFLAGS
    dd CHECKSUM
    ; Address fields (only used with the a.out kludge, which we don't use as an
    ; ELF kernel). They must still occupy these 5 dwords so the video fields
    ; below land at the correct offset (32) that GRUB reads.
    dd 0, 0, 0, 0, 0
    ; Video fields: mode_type, width, height, depth.
    dd 0                         ; 0 = linear graphics framebuffer (1 = text)
    dd 1024                      ; preferred width  (GRUB picks closest)
    dd 768                       ; preferred height
    dd 32                        ; preferred bits per pixel

section .bss
align 16
stack_bottom:
    resb 131072                  ; 128 KiB stack (DOOM's BSP recursion + large
                                 ; render locals need far more than the original
                                 ; 16 KiB; everything else is comfortable here)
stack_top:

section .text
global _start
extern kernel_main               ; kernel_main(uint32_t magic, uint32_t info)

_start:
    mov esp, stack_top

    ; GRUB leaves the Multiboot magic in EAX and the info struct pointer in EBX.
    ; Push them (C cdecl: right-to-left) so kernel_main(eax, ebx) sees them.
    push ebx                     ; arg 2: multiboot info pointer
    push eax                     ; arg 1: multiboot magic

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
