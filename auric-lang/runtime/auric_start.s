/* Auric app crt0: ARM9 entry stub.
 *
 * Modeled on AuroraOS's src/os/os_start.s. The loader copies the packed ARM9
 * payload to its load address (0x22000000) and branches to _start here, which
 * sets up a stack, zeroes .bss, then calls the program entry point, the Auric
 * `fn main`, emitted by the code generator as `au_main`.
 */
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

/* os_cache_sync(void): clean+invalidate the D-cache, invalidate the I-cache,
 * drain the write buffer. Same shape as AuroraOS's src/os/os_launch.s
 * (ARM946E-S: 4 KB / 4 ways / 32 B lines).
 *
 * The GPU driver (src/os/gpu9.c) is linked into Auric apps so that present()
 * is a GPU blit rather than a CPU copy, and it calls this before handing a
 * buffer to the ARM11, the GPU reads physical memory, so pending writes must
 * be out of the cache first. */
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
