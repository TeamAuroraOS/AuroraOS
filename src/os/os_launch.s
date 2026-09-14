/* Home Menu -> app hand-off. The Home Menu runs at 0x22000000, where apps also
 * load, so os_main.c relocates os_launch_stub to AURORA_APP_TRAMPOLINE_ADDR and
 * jumps to it; the stub copies the app into place, flushes the caches and
 * branches to it. The stub must stay position-independent: registers and
 * immediates only, no literal pool. */
.section .text
.arm
.align 2

.global os_launch_stub
.global os_launch_stub_end
.global os_cache_sync
.global os_return_stub
.global os_return_stub_end

/* Must match the AURORA_* constants in include/loader.h. */
.equ RET_DESC_ADDR, 0x25008000   @ [magic, os_image_size]
.equ OS_SNAPSHOT,   0x26000000   @ saved OS image
.equ OS_LOAD_ADDR,  0x22000000   @ where the OS runs / its entry (_os_start)

/* Whole-cache clean+invalidate by index (ARM946E-S: 4 KB, 4 ways, 32-byte
 * lines). */
.macro FLUSH_CACHES scratch0, scratch1
    mov   \scratch1, #0                  @ line index 0..0x3E0
1:
    mov   \scratch0, \scratch1           @ way 0 for this index
2:
    mcr   p15, 0, \scratch0, c7, c14, 2  @ clean+invalidate D-cache line (idx/seg)
    adds  \scratch0, \scratch0, #0x40000000 @ next way; carry set on wrap
    bcc   2b
    add   \scratch1, \scratch1, #0x20    @ next 32-byte line
    cmp   \scratch1, #0x400
    bne   1b
    mov   \scratch0, #0
    mcr   p15, 0, \scratch0, c7, c5, 0   @ invalidate entire I-cache
    mcr   p15, 0, \scratch0, c7, c10, 4  @ drain write buffer (DSB)
.endm

/* os_launch_stub(r0=src, r1=dst, r2=size, r3=entry): copy word-wise, flush the
 * caches, mask interrupts and branch to entry. Never returns. */
.type os_launch_stub, %function
os_launch_stub:
    add   r2, r2, #3
    bic   r2, r2, #3                     @ round size up to a multiple of 4
.Lcopy:
    cmp   r2, #0
    beq   .Lcopy_done
    ldr   r12, [r0], #4
    str   r12, [r1], #4
    subs  r2, r2, #4
    b     .Lcopy
.Lcopy_done:
    FLUSH_CACHES r0, r1                  @ r3 (entry) preserved
    mrs   r0, cpsr
    orr   r0, r0, #0xC0                  @ mask IRQ + FIQ
    msr   cpsr_c, r0
    bx    r3                             @ into the app
os_launch_stub_end:
.size os_launch_stub, .-os_launch_stub

/* os_cache_sync(void): clean+invalidate the D-cache, invalidate the I-cache,
 * drain the write buffer. */
.type os_cache_sync, %function
os_cache_sync:
    FLUSH_CACHES r0, r1
    bx    lr
.size os_cache_sync, .-os_cache_sync

/* os_return_stub(void): an app branches here (via AURORA_RETURN_STUB_ADDR) on
 * HOME. Restores the saved OS image, flushes the caches and jumps to the OS
 * entry. The Home Menu relocates it; its literal pool holds absolute addresses,
 * which relocation does not change. */
.type os_return_stub, %function
os_return_stub:
    ldr   r4, =RET_DESC_ADDR
    ldr   r2, [r4, #4]                @ OS image size in bytes
    ldr   r0, =OS_SNAPSHOT            @ src
    ldr   r1, =OS_LOAD_ADDR           @ dst
    mov   r3, r1                      @ entry = OS load address (_os_start)
    add   r2, r2, #3
    bic   r2, r2, #3                  @ round size up to a word
.Lrcopy:
    cmp   r2, #0
    beq   .Lrflush
    ldr   r12, [r0], #4
    str   r12, [r1], #4
    subs  r2, r2, #4
    b     .Lrcopy
.Lrflush:
    FLUSH_CACHES r0, r1               @ r3 (entry) preserved
    mrs   r0, cpsr
    orr   r0, r0, #0xC0               @ mask IRQ + FIQ
    msr   cpsr_c, r0
    bx    r3                          @ back into the OS
.ltorg                                @ keep the literal pool inside the stub
os_return_stub_end:
.size os_return_stub, .-os_return_stub

/* Data-only cache maintenance: clean without invalidating, so a framebuffer the
 * CPU just wrote stays cached, and leave the I-cache alone before a GPU blit. */

/* os_dcache_clean(void): write back every dirty D-cache line and drain; both
 * caches stay populated. */
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

/* os_dcache_clean_range(const void *addr, u32 len): clean just the lines
 * covering [addr, addr+len). Cheaper than the whole cache for the small shared
 * command blocks; for anything framebuffer-sized use os_dcache_clean. */
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

/* os_dcache_inval_range(addr, len): drop the lines covering the range without
 * writing them back. For memory the GPU just wrote; never for memory only the
 * CPU writes. */
.global os_dcache_inval_range
.type os_dcache_inval_range, %function
os_dcache_inval_range:
    add   r1, r0, r1                 @ end address
    bic   r0, r0, #31                @ round the start down to a line
1:
    mcr   p15, 0, r0, c7, c6, 1      @ invalidate D-cache line by MVA
    add   r0, r0, #32
    cmp   r0, r1
    blo   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 4     @ drain write buffer (DSB)
    bx    lr
.size os_dcache_inval_range, .-os_dcache_inval_range

/* os_dcache_flush(void): clean+invalidate the whole D-cache by index (128
 * operations) and drain; the I-cache is untouched. Above a few KB this beats a
 * range walk, which costs one operation per line of the range. */
.global os_dcache_flush
.type os_dcache_flush, %function
os_dcache_flush:
    mov   r1, #0                     @ line index 0..0x3E0
1:
    mov   r0, r1                     @ way 0 for this index
2:
    mcr   p15, 0, r0, c7, c14, 2     @ clean+invalidate D line (index/segment)
    adds  r0, r0, #0x40000000        @ next way; carry set on wrap
    bcc   2b
    add   r1, r1, #0x20              @ next 32-byte line
    cmp   r1, #0x400
    bne   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 4     @ drain write buffer (DSB)
    bx    lr
.size os_dcache_flush, .-os_dcache_flush

/* os_mpu_enable(void): switch on the protection unit and both caches. The firm
 * leaves the region table configured but the unit off. Region 5 (all FCRAM) is
 * cacheable and bufferable; VRAM and I/O are not. Both caches hold power-up
 * contents, so they are invalidated first. */
.global os_mpu_enable
.type os_mpu_enable, %function
os_mpu_enable:
    mov   r0, #0
    mcr   p15, 0, r0, c7, c5, 0      @ invalidate entire I-cache
    mcr   p15, 0, r0, c7, c6, 0      @ invalidate entire D-cache
    mcr   p15, 0, r0, c7, c10, 4     @ drain write buffer

    mrc   p15, 0, r0, c1, c0, 0
    orr   r0, r0, #0x1               @ bit 0  protection unit
    orr   r0, r0, #0x4               @ bit 2  D-cache
    orr   r0, r0, #0x8               @ bit 3  write buffer
    orr   r0, r0, #0x1000            @ bit 12 I-cache
    mcr   p15, 0, r0, c1, c0, 0

    mov   r0, #0
    mcr   p15, 0, r0, c7, c5, 0      @ flush the prefetch buffer
    bx    lr
.size os_mpu_enable, .-os_mpu_enable
