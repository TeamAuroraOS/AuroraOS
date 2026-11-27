.section .vectors, "ax"
.arm
.align 4
.global _start

/* Luma3DS and fastboot3DS start a FIRM with r0 = argc, r1 = argv and r2 =
 * 0xBEEF, argv[0] being the path it was loaded from. That string sits in the
 * loader's memory, likely on its stack, which is ours too (both end at
 * 0x08100000), so it is copied before anything uses the stack or clears
 * .bss. g_firm_argv0 stays empty for any other loader. */
_start:
    mov r4, r0
    mov r5, r1
    mov r6, r2

    mrs r0, cpsr
    orr r0, r0, #0xC0
    msr cpsr_c, r0

    ldr r3, =0xBEEF
    cmp r6, r3
    bne .Lno_argv
    cmp r4, #1
    blt .Lno_argv
    cmp r4, #8
    bgt .Lno_argv
    tst r5, #3
    bne .Lno_argv
    ldr r0, [r5]
    cmp r0, #0
    beq .Lno_argv
    ldr r1, =g_firm_argv0
    mov r2, #255
.Lcopy_argv:
    ldrb r3, [r0], #1
    strb r3, [r1], #1
    cmp r3, #0
    beq .Lno_argv
    subs r2, r2, #1
    bne .Lcopy_argv
    mov r3, #0
    strb r3, [r1]
.Lno_argv:

    ldr sp, =_stack_top

    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
.Lclear_bss:
    cmp r0, r1
    strlt r2, [r0], #4
    blt .Lclear_bss

    bl main

.Lhang:
    mcr p15, 0, r0, c7, c0, 4  @ WFI equivalent for ARMv5
    b .Lhang

.ltorg

.section .data
.align 2
.global g_firm_argv0
g_firm_argv0:
    .space 256
