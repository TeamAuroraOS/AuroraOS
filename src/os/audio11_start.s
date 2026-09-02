/*
 * ARM11 audio core entry (AuroraOS).
 *
 * The firm's ARM11 stub (src/arm11_start.s) wakes here via the mailbox after the
 * ARM9 OS copies this core to 0x23000000. Set up the stack, clear .bss, and call
 * the C entry. If it ever returns, wait forever.
 */
.section .start, "ax"
.arm
.align 4
.global _audio11_start

_audio11_start:
    cpsid   if                      @ mask IRQ + FIQ
    ldr     sp, =_audio11_stack_top

    ldr     r0, =_abss_start        @ clear .bss
    ldr     r1, =_abss_end
    mov     r2, #0
.Lbss:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     .Lbss

    bl      audio11_main

.Lhang:
    wfi
    b       .Lhang
.pool

/* ---- ARM11 exception stubs -----------------------------------------------
 * On an ARM11 fault, snapshot the state into the cross-core crash block
 * (CrashShared at 0x23380000; see include/crash_shared.h), set the magic last,
 * and hang. The ARM9 polls the magic and draws the crash screen. */
.equ CS_BASE, 0x23380000

.global crash_vec_undef11
.global crash_vec_pabt11
.global crash_vec_dabt11
.global crash_hang11

.type crash_vec_undef11, %function
crash_vec_undef11:
    ldr   sp, =(CS_BASE + 12)      @ &CrashShared.r[0]
    stmia sp, {r0-r12}
    ldr   r1, =CS_BASE
    sub   r0, lr, #4
    str   r0, [r1, #64]           @ pc
    mrs   r0, spsr
    str   r0, [r1, #68]           @ cpsr
    mov   r0, #0                  @ CRASH_UNDEF
    str   r0, [r1, #72]
    mov   r0, #0
    str   r0, [r1, #76]           @ dfsr
    str   r0, [r1, #80]           @ dfar
    b     crash_post11

.type crash_vec_pabt11, %function
crash_vec_pabt11:
    ldr   sp, =(CS_BASE + 12)
    stmia sp, {r0-r12}
    ldr   r1, =CS_BASE
    sub   r0, lr, #4
    str   r0, [r1, #64]
    mrs   r0, spsr
    str   r0, [r1, #68]
    mov   r0, #1                  @ CRASH_PABT
    str   r0, [r1, #72]
    mov   r0, #0
    str   r0, [r1, #76]
    str   r0, [r1, #80]
    b     crash_post11

.type crash_vec_dabt11, %function
crash_vec_dabt11:
    ldr   sp, =(CS_BASE + 12)
    stmia sp, {r0-r12}
    ldr   r1, =CS_BASE
    sub   r0, lr, #8
    str   r0, [r1, #64]
    mrs   r0, spsr
    str   r0, [r1, #68]
    mov   r0, #2                  @ CRASH_DABT
    str   r0, [r1, #72]
    mrc   p15, 0, r0, c5, c0, 0   @ DFSR
    str   r0, [r1, #76]
    mrc   p15, 0, r0, c6, c0, 0   @ DFAR
    str   r0, [r1, #80]
    b     crash_post11

crash_post11:                     @ r1 = CS_BASE
    mov   r0, #1
    str   r0, [r1, #4]            @ cpu = ARM11
    mrc   p15, 0, r0, c0, c0, 5   @ MPIDR
    and   r0, r0, #3
    str   r0, [r1, #8]           @ core id
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 0  @ clean entire D-cache
    mcr   p15, 0, r0, c7, c10, 4  @ DSB
    ldr   r0, =0x48535243         @ magic 'CRSH', written last
    str   r0, [r1, #0]
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 0
    mcr   p15, 0, r0, c7, c10, 4
.type crash_hang11, %function
crash_hang11:
    wfi
    b     crash_hang11
.pool
