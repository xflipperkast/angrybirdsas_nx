    .text
    .align 2
    .global guest_stack_call_asm
    .type guest_stack_call_asm, %function

guest_stack_call_asm:
    stp     x19, x30, [sp, #-16]!
    mov     x19, sp
    and     x9, x0, #0xfffffffffffffff0
    mov     sp, x9
    mov     x0, x2
    blr     x1
    mov     sp, x19
    ldp     x19, x30, [sp], #16
    ret

    .size guest_stack_call_asm, .-guest_stack_call_asm
