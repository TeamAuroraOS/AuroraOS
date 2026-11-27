.section .vectors, "ax"
.arm
.align 4
.global _start11

@ ARM11 handoff mailbox; must match AOS_ARM11_MAILBOX in include/loader.h. The
@ loader writes the payload's entry point here.
.equ ARM11_MAILBOX, 0x27000000

_start11:
    b       .Lboot

@ SCREEN11_STATE, SCREEN11_FLIP and SCREEN11_FLIPPED (include/screen11.h), at
@ 0x1FF80004, 0x1FF80008 and 0x1FF8000C.
    .word   0
    .word   0
    .word   0

.Lboot:
    cpsid   aif
    ldr     sp, =_stack11_top

    ldr     r0, =ARM11_MAILBOX
    mov     r1, #0
    str     r1, [r0]

    bl      screen11_setup
    bl      screen11_serve          @ returns the mailbox's entry point

    @ Flush caches (arm11_jump.s) and branch into it.
    ldr     r1, =aurora_jump_arm11
    bx      r1

.Lhang11:
    wfi
    b       .Lhang11
