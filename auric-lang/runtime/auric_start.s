/* Auric app crt0 -- ARM9 entry stub.
 *
 * Modeled on AuroraOS's src/os/os_start.s. The loader copies the packed ARM9
 * payload to its load address (0x22000000) and branches to _start here, which
 * sets up a stack, zeroes .bss, then calls the program entry point -- the Auric
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
