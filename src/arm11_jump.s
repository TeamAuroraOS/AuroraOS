.section .text
.arm
.align 2
.global aurora_jump_arm11
.type aurora_jump_arm11, %function

aurora_jump_arm11:
    mov     r12, r0
    mov     r0, #0
    mcr     p15, 0, r0, c7, c14, 0  @ clean+invalidate entire D-cache
    mcr     p15, 0, r0, c7, c10, 4  @ data synchronization barrier
    mcr     p15, 0, r0, c7, c5, 0   @ invalidate entire I-cache
    mcr     p15, 0, r0, c7, c5, 6   @ flush entire branch predictor
    mcr     p15, 0, r0, c7, c5, 4   @ prefetch flush (ISB)

    cpsid   if                      @ mask IRQ + FIQ
    bx      r12
.size aurora_jump_arm11, .-aurora_jump_arm11
