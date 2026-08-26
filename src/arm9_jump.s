.section .text
.arm
.align 2
.global aurora_jump_arm9
.type aurora_jump_arm9, %function

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
    bcc     .Lway                   @ ...loop until all 4 ways done
    add     r1, r1, #0x20           @ next cache line (32 bytes)
    cmp     r1, #0x400              @ 32 lines * 32 bytes per way
    bne     .Lline

    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0   @ invalidate entire I-cache
    mcr     p15, 0, r0, c7, c10, 4  @ drain write buffer (DSB)

    mrs     r0, cpsr                @ mask IRQ + FIQ before leaving the loader
    orr     r0, r0, #0xC0
    msr     cpsr_c, r0

    bx      r12                     @ into the payload
.size aurora_jump_arm9, .-aurora_jump_arm9
