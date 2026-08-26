/* Coded By DisLoPik for the AuroraOS Project. */
/*
 * ARM11 (MPCore, ARMv6K) cache-flush + branch stub.
 *
 * Reached from the ARM11 spin loop once the ARM9 loader has posted an entry
 * point in the handoff mailbox. Unlike the ARM946E-S, ARMv6 has whole-cache
 * operations, so we can clean+invalidate the D-cache in one op, then invalidate
 * the I-cache and branch predictor before jumping. This path is only exercised
 * once an AOS1 image actually carries an ARM11 payload.
 */
.section .text
.arm
.align 2
.global aurora_jump_arm11
.type aurora_jump_arm11, %function

@ void aurora_jump_arm11(uint32_t entry);  -- r0 = entry point, never returns
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
