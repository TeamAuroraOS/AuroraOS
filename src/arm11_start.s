.section .vectors, "ax"
.arm
.align 4
.global _start11

@ Fixed ARM11 handoff mailbox in FCRAM. MUST match AOS_ARM11_MAILBOX in
@ include/loader.h -- the ARM9 loader writes the ARM11 payload's entry point
@ here once it has copied the payload into RAM.
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
