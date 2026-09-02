/*
 * AuroraOS ARM9 exception entry stubs + register capture.
 *
 * Each vector stub snapshots the pre-exception registers into g_crash_dump,
 * records the faulting PC (adjusted per exception type) and the reason, switches
 * to a private crash stack, and calls crash_handle() (which never returns).
 *
 * Dump layout (must match CrashDump in include/crash.h):
 *   r0-r12 -> offset 0..48, pc -> 52, cpsr -> 56, exc -> 60, dfsr -> 64, dfar 68
 */
.section .text
.arm
.align 2

.global crash_vec_undef
.global crash_vec_pabt
.global crash_vec_dabt
.global crash_capture
.global crash_hang
.extern g_crash_dump
.extern crash_handle

.type crash_vec_undef, %function
crash_vec_undef:
    ldr   sp, =g_crash_dump
    stmia sp, {r0-r12}          @ r0-r12 (pre-exception)
    sub   r0, lr, #4            @ Undef: faulting pc = lr - 4
    str   r0, [sp, #52]
    mrs   r0, spsr
    str   r0, [sp, #56]         @ cpsr
    mov   r0, #0                @ CRASH_UNDEF
    str   r0, [sp, #60]
    mov   r0, #0
    str   r0, [sp, #64]         @ dfsr = 0
    str   r0, [sp, #68]         @ dfar = 0
    str   r0, [sp, #72]         @ cpu = ARM9
    str   r0, [sp, #76]         @ core = 0
    mov   r0, sp               @ r0 = &g_crash_dump
    ldr   sp, =_crash_stack_top
    bl    crash_handle
.Lundef_hang:
    b     .Lundef_hang

.type crash_vec_pabt, %function
crash_vec_pabt:
    ldr   sp, =g_crash_dump
    stmia sp, {r0-r12}
    sub   r0, lr, #4           @ Prefetch Abort: faulting pc = lr - 4
    str   r0, [sp, #52]
    mrs   r0, spsr
    str   r0, [sp, #56]
    mov   r0, #1               @ CRASH_PABT
    str   r0, [sp, #60]
    mov   r0, #0
    str   r0, [sp, #64]
    str   r0, [sp, #68]
    mov   r0, #0
    str   r0, [sp, #72]         @ cpu = ARM9
    str   r0, [sp, #76]         @ core = 0
    mov   r0, sp
    ldr   sp, =_crash_stack_top
    bl    crash_handle
.Lpabt_hang:
    b     .Lpabt_hang

.type crash_vec_dabt, %function
crash_vec_dabt:
    ldr   sp, =g_crash_dump
    stmia sp, {r0-r12}
    sub   r0, lr, #8           @ Data Abort: faulting pc = lr - 8
    str   r0, [sp, #52]
    mrs   r0, spsr
    str   r0, [sp, #56]
    mov   r0, #2               @ CRASH_DABT
    str   r0, [sp, #60]
    mrc   p15, 0, r0, c5, c0, 0 @ DFSR (data fault status)
    str   r0, [sp, #64]
    mrc   p15, 0, r0, c6, c0, 0 @ DFAR (data fault address)
    str   r0, [sp, #68]
    mov   r0, #0
    str   r0, [sp, #72]         @ cpu = ARM9
    str   r0, [sp, #76]         @ core = 0
    mov   r0, sp
    ldr   sp, =_crash_stack_top
    bl    crash_handle
.Ldabt_hang:
    b     .Ldabt_hang

/* crash_capture(CrashDump *d in r0): fill d with the current register state. */
.type crash_capture, %function
crash_capture:
    stmia r0, {r0-r12}         @ r[0]=d ptr, r[1..12]=live values
    str   lr, [r0, #52]        @ pc = caller
    mrs   r1, cpsr
    str   r1, [r0, #56]
    bx    lr

/* Landing pad for the vectors we don't handle (they don't occur while the OS
 * runs: IRQ/FIQ are masked and SWI/reset are unused). */
.type crash_hang, %function
crash_hang:
    b     crash_hang

.pool

.section .bss
.align 3
_crash_stack:
    .space 4096
_crash_stack_top:
