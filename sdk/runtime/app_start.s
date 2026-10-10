/* Native app crt0. The app header aurcc.py generates (.aurhead: a branch, the
 * "AURICON1" icon block) comes first and branches here; app_entry() in
 * app_main.c then sets up the runtime and calls main(). */
.section .start, "ax"
.arm
.global _start

_start:
    mrs   r0, cpsr
    orr   r0, r0, #0xC0          @ keep IRQ + FIQ masked
    msr   cpsr_c, r0

    ldr   sp, =_app_stack_top

    ldr   r0, =_app_bss_start
    ldr   r1, =_app_bss_end
    mov   r2, #0
.Lbss:
    cmp   r0, r1
    strlt r2, [r0], #4
    blt   .Lbss

    bl    app_entry

.Lhang:
    b     .Lhang
.pool

/* newlib's __libc_fini_array calls _fini after the .fini_array functions;
 * there is nothing else to run. */
.section .text._init_fini, "ax"
.global _init
.global _fini
.type _init, %function
.type _fini, %function
_init:
_fini:
    bx    lr

/* The cache maintenance Gpu9.c and crash.c call; the OS has them in
 * os_launch.s, which apps do not link. A launched app inherits the OS's
 * caches, which are on. */

/* os_cache_sync(void): clean+invalidate the D-cache, invalidate the I-cache,
 * drain the write buffer (ARM946E-S: 4 KB, 4 ways, 32-byte lines). */
.section .text.os_cache_sync, "ax"
.global os_cache_sync
.type os_cache_sync, %function
os_cache_sync:
    mov   r1, #0
1:
    mov   r0, r1
2:
    mcr   p15, 0, r0, c7, c14, 2     @ clean+invalidate D-cache line (idx/seg)
    adds  r0, r0, #0x40000000        @ next way; carry set on wrap
    bcc   2b
    add   r1, r1, #0x20
    cmp   r1, #0x400
    bne   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c5, 0      @ invalidate entire I-cache
    mcr   p15, 0, r0, c7, c10, 4     @ drain write buffer
    bx    lr
.size os_cache_sync, .-os_cache_sync

/* os_dcache_clean(void): write back every dirty D-cache line. */
.section .text.os_dcache_clean, "ax"
.global os_dcache_clean
.type os_dcache_clean, %function
os_dcache_clean:
    mov   r1, #0
1:
    mov   r0, r1
2:
    mcr   p15, 0, r0, c7, c10, 2     @ clean D-cache line (index/segment)
    adds  r0, r0, #0x40000000
    bcc   2b
    add   r1, r1, #0x20
    cmp   r1, #0x400
    bne   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 4
    bx    lr
.size os_dcache_clean, .-os_dcache_clean

/* os_dcache_clean_range(const void *addr, u32 len) */
.section .text.os_dcache_clean_range, "ax"
.global os_dcache_clean_range
.type os_dcache_clean_range, %function
os_dcache_clean_range:
    add   r1, r0, r1
    bic   r0, r0, #31
1:
    mcr   p15, 0, r0, c7, c10, 1     @ clean D-cache line by MVA
    add   r0, r0, #32
    cmp   r0, r1
    blo   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 4
    bx    lr
.size os_dcache_clean_range, .-os_dcache_clean_range
