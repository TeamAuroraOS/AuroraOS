.section .vectors, "ax"
.arm
.align 4
.global _start11

@ ARM11 handoff mailbox; must match AOS_ARM11_MAILBOX in include/loader.h. The
@ loader writes the payload's entry point here.
.equ ARM11_MAILBOX, 0x27000000

_start11:
    cpsid   aif
    ldr     sp, =_stack11_top

    ldr     r0, =ARM11_MAILBOX
    mov     r1, #0
    str     r1, [r0]
.Lspin:
    ldr     r1, [r0]
    cmp     r1, #0
    beq     .Lspin

    @ Entry point received: flush caches (arm11_jump.s) and branch into it.
    mov     r0, r1
    ldr     r1, =aurora_jump_arm11
    bx      r1

.Lhang11:
    wfi
    b       .Lhang11
