

@ Coded By DisLoPik for the AuroraOS Project.
.section .vectors, "ax"
.arm
.align 4
.global _start11

_start11:
    
    cpsid aif

    
    ldr sp, =_stack11_top

    
    ldr r0, =arm11_entry_func
.Lspin:
    ldr r1, [r0]
    cmp r1, #0
    beq .Lspin

    
    blx r1

    
.Lhang11:
    wfi
    b .Lhang11

.section .bss
.align 4
.global arm11_entry_func
arm11_entry_func:
    .word 0
