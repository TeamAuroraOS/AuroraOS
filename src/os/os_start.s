.section .start, "ax"
.arm
.global _os_start

_os_start:
    mrs   r0, cpsr
    orr   r0, r0, #0xC0          @ keep IRQ + FIQ masked
    msr   cpsr_c, r0

    ldr   sp, =_os_stack_top

    ldr   r0, =_os_bss_start     @ clear .bss
    ldr   r1, =_os_bss_end
    mov   r2, #0
.Lbss:
    cmp   r0, r1
    strlt r2, [r0], #4
    blt   .Lbss

    bl    os_main

.Lhang:
    b     .Lhang
.pool
