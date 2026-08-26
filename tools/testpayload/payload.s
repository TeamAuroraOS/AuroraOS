/* Coded By DisLoPik for the AuroraOS Project. */
/*
 * Throwaway ARM9 test payload (Phase 7).
 *
 * The smallest thing that proves the whole loader pipeline end to end: it fills
 * both 3DS framebuffers with solid green, then spins. If "Boot Aurora" turns the
 * screens green, then SD read -> FAT -> copy to 0x22000000 -> cache flush ->
 * jump all work. Linked to run in place at 0x22000000 (see payload.ld); entry
 * is _start at that address.
 *
 * Framebuffers (from include/aurora.h): top at 0x18300000 (400*240*3 bytes),
 * bottom at 0x18346500 -- they are contiguous, so one fill covers both. Pixel
 * byte order is B,G,R, so (0x00,0xFF,0x00) is green for every pixel regardless
 * of the rotated framebuffer layout.
 */
.section .start, "ax"
.arm
.global _start

_start:
    ldr   r0, =0x18300000                        @ top framebuffer base
    ldr   r1, =(400*240*3 + 320*240*3)           @ bytes for both screens
    mov   r2, #0x00                              @ B
    mov   r3, #0xFF                              @ G
    mov   r4, #0x00                              @ R
.Lfill:
    strb  r2, [r0], #1
    strb  r3, [r0], #1
    strb  r4, [r0], #1
    subs  r1, r1, #3
    bgt   .Lfill
.Lhang:
    b     .Lhang
.ltorg
