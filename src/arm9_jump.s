.section .text
.arm
.align 2
.global aurora_jump_arm9
.type aurora_jump_arm9, %function

@ Leaves with the protection unit and both caches off, as the firm found them,
@ so the OS starts the way it always has (it turns them on itself, unless
@ SELECT is held).
aurora_jump_arm9:
    mov     r12, r0                 @ preserve the entry point

    @ Clean & invalidate the entire D-cache by index/segment.
    @ 4 KB / 4 ways / 32-byte lines => 32 lines per way (index 0x00..0x3E0),
    @ way number in bits [31:30].
    mov     r1, #0                  @ line index
.Lline:
    mov     r0, r1                  @ start at way 0 for this index
.Lway:
    mcr     p15, 0, r0, c7, c14, 2  @ clean+invalidate D-cache line (index/seg)
    adds    r0, r0, #0x40000000     @ advance to next way; carry set when it wraps
    bcc     .Lway
    add     r1, r1, #0x20           @ next cache line (32 bytes)
    cmp     r1, #0x400              @ 32 lines * 32 bytes per way
    bne     .Lline

    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4  @ drain write buffer (DSB)

    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #0x1000         @ I-cache
    bic     r0, r0, #0x5            @ D-cache, protection unit
    mcr     p15, 0, r0, c1, c0, 0

    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0   @ invalidate entire I-cache
    mcr     p15, 0, r0, c7, c6, 0   @ invalidate entire D-cache
    mcr     p15, 0, r0, c7, c10, 4  @ drain write buffer (DSB)

    mrs     r0, cpsr                @ mask IRQ + FIQ before leaving the loader
    orr     r0, r0, #0xC0
    msr     cpsr_c, r0

    bx      r12
.size aurora_jump_arm9, .-aurora_jump_arm9

@ fx_mpu_on(void): the protection unit and both caches on, with Luma3DS's
@ region table (arm9/source/start.s), which is also the one the OS has always
@ found. Only FCRAM, ARM9 RAM and the boot ROM are cached, so the firm's
@ ARM11 sees AXI WRAM and VRAM writes without a cache clean. VRAM is also
@ write-buffered here, which Luma3DS's table does not do; fx_handoff() puts
@ that back.
.global fx_mpu_on
.type fx_mpu_on, %function
fx_mpu_on:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0   @ invalidate entire I-cache
    mcr     p15, 0, r0, c7, c6, 0   @ invalidate entire D-cache
    mcr     p15, 0, r0, c7, c10, 4  @ drain write buffer

    ldr     r0, =0x33333333         @ read/write in every region
    mcr     p15, 0, r0, c5, c0, 2   @ data
    mcr     p15, 0, r0, c5, c0, 3   @ instructions

    ldr     r0, =0xFFFF001F         @ 0: boot ROM, 64 KB
    mcr     p15, 0, r0, c6, c0, 0
    ldr     r0, =0x3000801B         @ 1: DTCM, 16 KB
    mcr     p15, 0, r0, c6, c1, 0
    ldr     r0, =0x01FF801D         @ 2: ITCM, 32 KB
    mcr     p15, 0, r0, c6, c2, 0
    ldr     r0, =0x08000029         @ 3: ARM9 RAM, 2 MB
    mcr     p15, 0, r0, c6, c3, 0
    ldr     r0, =0x10000029         @ 4: ARM9 I/O, 2 MB
    mcr     p15, 0, r0, c6, c4, 0
    ldr     r0, =0x20000037         @ 5: FCRAM, 256 MB
    mcr     p15, 0, r0, c6, c5, 0
    ldr     r0, =0x1FF00027         @ 6: DSP RAM and AXI WRAM, 1 MB
    mcr     p15, 0, r0, c6, c6, 0
    ldr     r0, =0x1800002D         @ 7: VRAM, 8 MB
    mcr     p15, 0, r0, c6, c7, 0

    mov     r0, #0xA9               @ regions 0, 3, 5 and 7
    mcr     p15, 0, r0, c3, c0, 0   @ write bufferable
    mov     r0, #0x29               @ regions 0, 3 and 5
    mcr     p15, 0, r0, c2, c0, 0   @ data cacheable
    mcr     p15, 0, r0, c2, c0, 1   @ instruction cacheable

    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #0x1            @ protection unit
    orr     r0, r0, #0x4            @ D-cache
    orr     r0, r0, #0x8            @ write buffer
    orr     r0, r0, #0x1000         @ I-cache
    mcr     p15, 0, r0, c1, c0, 0

    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0   @ flush the prefetch buffer
    bx      lr
.size fx_mpu_on, .-fx_mpu_on

@ fx_dcache_clean(void): write every dirty D-cache line back and drain, for
@ memory another CPU is about to read.
.global fx_dcache_clean
.type fx_dcache_clean, %function
fx_dcache_clean:
    mov     r1, #0
1:
    mov     r0, r1
2:
    mcr     p15, 0, r0, c7, c10, 2  @ clean D-cache line (index/segment)
    adds    r0, r0, #0x40000000
    bcc     2b
    add     r1, r1, #0x20
    cmp     r1, #0x400
    bne     1b
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 4  @ drain write buffer
    bx      lr
.size fx_dcache_clean, .-fx_dcache_clean

@ fx_copy(dst, src, bytes): word-aligned buffers, bytes a multiple of 16
@ (a framebuffer column is 720).
.global fx_copy
.type fx_copy, %function
fx_copy:
    cmp     r2, #0
    bxle    lr
    push    {r4, r5}
1:
    ldmia   r1!, {r3, r4, r5, r12}
    stmia   r0!, {r3, r4, r5, r12}
    subs    r2, r2, #16
    bgt     1b
    pop     {r4, r5}
    bx      lr
.size fx_copy, .-fx_copy

.ltorg
