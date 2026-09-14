/* Auric app crt0: set up the stack, clear .bss and call au_main (the Auric `fn
 * main`). */
.section .start, "ax"
.arm
.global _start

_start:
    mrs   r0, cpsr
    orr   r0, r0, #0xC0          @ keep IRQ + FIQ masked
    msr   cpsr_c, r0

    ldr   sp, =_auric_stack_top

    ldr   r0, =_auric_bss_start  @ clear .bss
    ldr   r1, =_auric_bss_end
    mov   r2, #0
.Lbss:
    cmp   r0, r1
    strlt r2, [r0], #4
    blt   .Lbss

    bl    au_main                @ Auric `fn main`

.Lhang:
    b     .Lhang
.pool

/* os_cache_sync(void): clean+invalidate the D-cache, invalidate the I-cache and
 * drain. Gpu9.c calls it before the GPU reads a buffer. */
.global os_cache_sync
.type os_cache_sync, %function
os_cache_sync:
    mov   r1, #0                     @ line index 0..0x3E0
1:
    mov   r0, r1                     @ way 0 for this index
2:
    mcr   p15, 0, r0, c7, c14, 2     @ clean+invalidate D-cache line (idx/seg)
    adds  r0, r0, #0x40000000        @ next way; carry set on wrap
    bcc   2b
    add   r1, r1, #0x20              @ next 32-byte line
    cmp   r1, #0x400
    bne   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c5, 0      @ invalidate entire I-cache
    mcr   p15, 0, r0, c7, c10, 4     @ drain write buffer (DSB)
    bx    lr
.size os_cache_sync, .-os_cache_sync

/* os_dcache_clean and os_dcache_clean_range: the data-only cache maintenance
 * Gpu9.c uses. Apps link Gpu9.c but not os_launch.s, so they carry their own
 * copies; a launched app inherits the OS's caches, which are on. */
.global os_dcache_clean
.type os_dcache_clean, %function
os_dcache_clean:
    mov   r1, #0                     @ line index 0..0x3E0
1:
    mov   r0, r1                     @ way 0 for this index
2:
    mcr   p15, 0, r0, c7, c10, 2     @ clean D-cache line (index/segment)
    adds  r0, r0, #0x40000000        @ next way; carry set on wrap
    bcc   2b
    add   r1, r1, #0x20              @ next 32-byte line
    cmp   r1, #0x400
    bne   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 4     @ drain write buffer (DSB)
    bx    lr
.size os_dcache_clean, .-os_dcache_clean

.global os_dcache_clean_range
.type os_dcache_clean_range, %function
os_dcache_clean_range:
    add   r1, r0, r1                 @ end address
    bic   r0, r0, #31                @ round the start down to a line
1:
    mcr   p15, 0, r0, c7, c10, 1     @ clean D-cache line by MVA
    add   r0, r0, #32
    cmp   r0, r1
    blo   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 4     @ drain write buffer (DSB)
    bx    lr
.size os_dcache_clean_range, .-os_dcache_clean_range
