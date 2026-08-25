

@ Coded By DisLoPik for the AuroraOS Project.
.section .vectors, "ax"
.arm
.align 4
.global _start

_start:
    
    mrs r0, cpsr
    orr r0, r0, #0xC0
    msr cpsr_c, r0

    
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
