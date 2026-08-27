/*
 * AuroraOS Home Menu -> app hand-off (ARM9).
 *
 * The running Home Menu lives at AOS_ARM9_LOAD_ADDR (0x22000000), which is also
 * where AUR1 apps load, so it cannot copy an app onto itself in place. Instead
 * os_main.c relocates os_launch_stub to scratch (AURORA_APP_TRAMPOLINE_ADDR),
 * calls os_cache_sync() so the copy is fetchable, then jumps into the relocated
 * stub. The stub -- now running clear of both the staged payload and the load
 * region -- performs the final copy, flushes caches, and branches into the app.
 *
 * os_launch_stub is position-independent: it uses only registers and immediate
 * operands (no literal pool with absolute addresses), so it runs correctly from
 * wherever it is relocated.
 */
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

/* ---- cache maintenance shared shape (ARM946E-S: 4 KB / 4 ways / 32 B lines) */
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

/* os_launch_stub(r0=src, r1=dst, r2=size_bytes, r3=entry)
 *   Copies size bytes src->dst (word-wise, rounded up to a word), flushes the
 *   caches, masks interrupts, and branches to entry. Never returns. */
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

/* os_cache_sync(void): clean+invalidate D-cache, invalidate I-cache, drain.
 *   Called after relocating the stub so the copied instructions are fetched
 *   from memory rather than a stale I-cache line. */
.type os_cache_sync, %function
os_cache_sync:
    FLUSH_CACHES r0, r1
    bx    lr
.size os_cache_sync, .-os_cache_sync

/* os_return_stub(void): the app branches here (via AURORA_RETURN_STUB_ADDR) on
 * a HOME press. Restores the saved OS image (OS_SNAPSHOT -> OS_LOAD_ADDR, size
 * from the descriptor), flushes caches, and jumps to the OS entry, which brings
 * the Home Menu back. Never returns. The Home Menu relocates this stub to
 * AURORA_RETURN_STUB_ADDR; its literal pool holds absolute addresses, which
 * survive relocation (the pc-relative loads stay valid, the values do not
 * change). */
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
