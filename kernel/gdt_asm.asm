section .text
global gdt_flush
global tss_flush

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload_cs    ; far jump reloads CS with the kernel code selector
.reload_cs:
    ret

tss_flush:
    mov ax, 0x28
    ltr ax
    ret
