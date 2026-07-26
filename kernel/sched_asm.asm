section .text
global sched_switch

sched_switch:
    mov eax, [esp + 4]     ; old_esp (where to save current esp)
    mov edx, [esp + 8]

    pushfd
    push ebx
    push esi
    push edi
    push ebp

    mov [eax], esp
    mov esp, edx

    pop ebp
    pop edi
    pop esi
    pop ebx
    popfd
    ret
