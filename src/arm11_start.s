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

    @ Backlights. The ARM9 cannot reach the LCD registers, so set them here the
    @ way Core11.c's backlight11_set does: PWM period 575 with the enable bit,
    @ adaptive backlight (+0x00 bit 0) off, and the calibrated duty for level 4
    @ of 5 (top 75, bottom 162; the top panel needs less drive). With ABL left
    @ on, a dark top screen is dimmed by the hardware.
    ldr     r3, =0x1023E

    ldr     r0, =0x10202200         @ top LCD
    str     r3, [r0, #0x44]
    ldr     r2, [r0]
    bic     r2, r2, #1
    str     r2, [r0]
    mov     r1, #75
    str     r1, [r0, #0x40]

    ldr     r0, =0x10202A00         @ bottom LCD
    str     r3, [r0, #0x44]
    ldr     r2, [r0]
    bic     r2, r2, #1
    str     r2, [r0]
    mov     r1, #162
    str     r1, [r0, #0x40]

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
