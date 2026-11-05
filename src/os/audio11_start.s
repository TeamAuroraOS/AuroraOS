/* ARM11 core entry: set up the stack, clear .bss and call audio11_main. */
.section .start, "ax"
.arm
.align 4
.global _audio11_start

_audio11_start:
    cpsid   if                      @ mask IRQ + FIQ
    ldr     sp, =_audio11_stack_top

    ldr     r0, =_abss_start        @ clear .bss
    ldr     r1, =_abss_end
    mov     r2, #0
.Lbss:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     .Lbss

    bl      audio11_main

.Lhang:
    wfi
    b       .Lhang
.pool

/* mmu11_stop(r0 = L2 on): everything cached reaches memory, then the MMU and
 * caches go off. Nothing here touches data memory, since a dirty line made after
 * the clean would be lost. Fetches and literals only read. */
.global mmu11_stop
.type mmu11_stop, %function
mmu11_stop:
    mov   r1, #0
    mcr   p15, 0, r1, c7, c14, 0  @ clean + invalidate D
    mcr   p15, 0, r1, c7, c10, 4  @ DSB
    cmp   r0, #0
    beq   .Lstop_map
    ldr   r2, =0x17E107FC         @ L2C clean + invalidate by way
    ldr   r3, =0xFFFF
    str   r3, [r2]
    ldr   r12, =0x4000000
.Lstop_way:
    ldr   r3, [r2]
    cmp   r3, #0
    beq   .Lstop_sync
    subs  r12, r12, #1
    bne   .Lstop_way
.Lstop_sync:
    ldr   r2, =0x17E10730         @ L2C cache sync
    str   r1, [r2]
    ldr   r12, =0x4000000
.Lstop_synced:
    ldr   r3, [r2]
    tst   r3, #1
    beq   .Lstop_map
    subs  r12, r12, #1
    bne   .Lstop_synced
.Lstop_map:
    mrc   p15, 0, r2, c1, c0, 0
    ldr   r3, =0x1805             @ M, C, Z, I
    bic   r2, r2, r3
    mcr   p15, 0, r2, c1, c0, 0
    mcr   p15, 0, r1, c7, c5, 4   @ ISB
    cmp   r0, #0
    ldrne r2, =0x17E10100         @ L2C control
    strne r1, [r2]
    mcr   p15, 0, r1, c7, c7, 0   @ invalidate I and D
    mcr   p15, 0, r1, c7, c5, 6   @ branch predictor
    mcr   p15, 0, r1, c8, c7, 0   @ TLB
    mcr   p15, 0, r1, c7, c10, 4  @ DSB
    mcr   p15, 0, r1, c7, c5, 4   @ ISB
    bx    lr
.pool

/* The other cores' loop, copied to SMP_STUB_ADDR (Smp11.c, include/smp.h). It
 * uses no stack and only PC-relative literals. A core enters it from the boot
 * ROM and comes back to .Lsmp_idle after each job, and it waits in WFI, since a
 * clock mode change only applies with every core in WFI.
 *
 * SmpBlock offsets: alive 0x00, entry 0x10, arg 0x20, sp 0x30, running 0x40. */
.global smp11_stub
.global smp11_stub_end
.type smp11_stub, %function
smp11_stub:
    cpsid aif
    ldr   r3, .Lsmp_sctlr         @ the MMU and D-cache off, whatever boot11 left
    mov   r4, #0
    mcr   p15, 0, r3, c1, c0, 0
    mcr   p15, 0, r4, c1, c0, 1   @ auxiliary control: not in SMP mode yet
    mcr   p15, 0, r4, c7, c7, 0   @ invalidate I and D
    mcr   p15, 0, r4, c7, c10, 4  @ DSB
    mcr   p15, 0, r4, c7, c5, 4   @ ISB
    clrex
    mrc   p15, 0, r4, c0, c0, 5   @ MPIDR: this core's number
    and   r4, r4, #3
    ldr   r5, .Lsmp_block
    add   r5, r5, r4, lsl #2      @ r5 + an offset is this core's word
    ldr   r6, .Lsmp_gicc
    mov   r0, #1
    str   r0, [r6, #0x00]         @ this core's CPU interface on
    mov   r0, #0xF0
    str   r0, [r6, #0x04]         @ every priority passes, so WFI can wake
    cmp   r4, #2
    blo   .Lsmp_idle
    ldr   r0, .Lsmp_cpu_cnt       @ cores 2 and 3 tell core 0 they left boot11
    mov   r1, #1                  @ out of reset, data overlay off
    strb  r1, [r0, r4]
.Lsmp_idle:
    ldr   r0, .Lsmp_alive
    str   r0, [r5, #0x00]
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 4  @ DSB
.Lsmp_poll:
    ldr   r0, [r5, #0x10]
    cmp   r0, #0
    bne   .Lsmp_go
    wfi
    ldr   r1, [r6, #0x0C]         @ acknowledge whatever woke it
    str   r1, [r6, #0x10]
    b     .Lsmp_idle              @ says it is here again, which a ping asks
.Lsmp_go:
    mov   r7, r0
    mov   r1, #1
    str   r1, [r5, #0x40]         @ running, before entry clears
    mov   r1, #0
    str   r1, [r5, #0x10]
    str   r1, [r5, #0x00]
    ldr   r8, [r5, #0x20]
    ldr   sp, [r5, #0x30]
    mcr   p15, 0, r1, c7, c7, 0   @ invalidate I and D
    mcr   p15, 0, r1, c8, c7, 0   @ TLB
    mcr   p15, 0, r1, c7, c10, 4  @ DSB
    mrc   p15, 0, r0, c1, c0, 1
    orr   r0, r0, #0x2F           @ branch prediction, folding, SMP mode
    mcr   p15, 0, r0, c1, c0, 1
    mvn   r0, #0
    mcr   p15, 0, r0, c3, c0, 0   @ domains: manager, nothing is checked
    mcr   p15, 0, r1, c2, c0, 2
    ldr   r0, .Lsmp_tt
    mcr   p15, 0, r0, c2, c0, 0
    mov   r0, #0xF00000
    mcr   p15, 0, r0, c1, c0, 2   @ cp10 and cp11
    mcr   p15, 0, r1, c7, c5, 4   @ ISB
    mov   r0, #0x40000000
    mcr   p10, 7, r0, c8, c0, 0   @ FPEXC.EN
    mov   r0, #0x3000000
    mcr   p10, 7, r0, c1, c0, 0   @ FPSCR: RunFast, there is no support code
    mrc   p15, 0, r0, c1, c0, 0
    ldr   r2, .Lsmp_on            @ M, C, Z, I, U, XP
    orr   r0, r0, r2
    mcr   p15, 0, r0, c1, c0, 0
    mcr   p15, 0, r1, c7, c5, 4   @ ISB
    mov   r0, r8
    mov   r1, r4
    blx   r7
    mov   r1, #0
    mcr   p15, 0, r1, c7, c14, 0  @ clean + invalidate D
    mcr   p15, 0, r1, c7, c10, 4  @ DSB
    mrc   p15, 0, r0, c1, c0, 0
    ldr   r2, .Lsmp_off           @ M, C, Z, I
    bic   r0, r0, r2
    mcr   p15, 0, r0, c1, c0, 0
    mcr   p15, 0, r1, c7, c5, 4   @ ISB
    mcr   p15, 0, r1, c7, c14, 0  @ anything left in the moment the cache was on
    mcr   p15, 0, r1, c7, c10, 4  @ DSB
    mrc   p15, 0, r0, c1, c0, 1
    bic   r0, r0, #0x20           @ out of SMP mode, with nothing in the cache
    mcr   p15, 0, r0, c1, c0, 1
    mcr   p15, 0, r1, c7, c7, 0   @ invalidate I and D
    mcr   p15, 0, r1, c8, c7, 0   @ TLB
    mcr   p15, 0, r1, c7, c10, 4  @ DSB
    str   r1, [r5, #0x40]
    b     .Lsmp_idle
.Lsmp_sctlr:
    .word 0x00055078
.Lsmp_on:
    .word 0x00C01805
.Lsmp_off:
    .word 0x00001805
.Lsmp_block:
    .word 0x233E2000              @ SMP_BLOCK_ADDR
.Lsmp_tt:
    .word 0x233E4000              @ SMP_TT_ADDR
.Lsmp_gicc:
    .word 0x17E00100
.Lsmp_cpu_cnt:
    .word 0x10141310              @ PDN_LGR_CPU_CNT
.Lsmp_alive:
    .word 0x50494D53              @ SMP_ALIVE
smp11_stub_end:

/* ARM11 exception stubs: snapshot the state into the cross-core crash block
 * (include/crash_shared.h), set the magic last and hang; the ARM9 draws the
 * crash screen. */
.equ CS_BASE, 0x23380000

.global crash_vec_undef11
.global crash_vec_pabt11
.global crash_vec_dabt11
.global crash_hang11

.type crash_vec_undef11, %function
crash_vec_undef11:
    ldr   sp, =(CS_BASE + 12)      @ &CrashShared.r[0]
    stmia sp, {r0-r12}
    ldr   r1, =CS_BASE
    sub   r0, lr, #4
    str   r0, [r1, #64]           @ pc
    mrs   r0, spsr
    str   r0, [r1, #68]           @ cpsr
    mov   r0, #0                  @ CRASH_UNDEF
    str   r0, [r1, #72]
    mov   r0, #0
    str   r0, [r1, #76]           @ dfsr
    str   r0, [r1, #80]           @ dfar
    b     crash_post11

.type crash_vec_pabt11, %function
crash_vec_pabt11:
    ldr   sp, =(CS_BASE + 12)
    stmia sp, {r0-r12}
    ldr   r1, =CS_BASE
    sub   r0, lr, #4
    str   r0, [r1, #64]
    mrs   r0, spsr
    str   r0, [r1, #68]
    mov   r0, #1                  @ CRASH_PABT
    str   r0, [r1, #72]
    mov   r0, #0
    str   r0, [r1, #76]
    str   r0, [r1, #80]
    b     crash_post11

.type crash_vec_dabt11, %function
crash_vec_dabt11:
    ldr   sp, =(CS_BASE + 12)
    stmia sp, {r0-r12}
    ldr   r1, =CS_BASE
    sub   r0, lr, #8
    str   r0, [r1, #64]
    mrs   r0, spsr
    str   r0, [r1, #68]
    mov   r0, #2                  @ CRASH_DABT
    str   r0, [r1, #72]
    mrc   p15, 0, r0, c5, c0, 0   @ DFSR
    str   r0, [r1, #76]
    mrc   p15, 0, r0, c6, c0, 0   @ DFAR
    str   r0, [r1, #80]
    b     crash_post11

crash_post11:                     @ r1 = CS_BASE
    mov   r0, #1
    str   r0, [r1, #4]            @ cpu = ARM11
    mrc   p15, 0, r0, c0, c0, 5   @ MPIDR
    and   r0, r0, #3
    str   r0, [r1, #8]           @ core id
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 0  @ clean entire D-cache
    mcr   p15, 0, r0, c7, c10, 4  @ DSB
    ldr   r0, =0x48535243         @ magic 'CRSH', written last
    str   r0, [r1, #0]
    mov   r0, #0
    mcr   p15, 0, r0, c7, c10, 0
    mcr   p15, 0, r0, c7, c10, 4
    @ Start the crash beep rendered at boot. Register stores only: there is no
    @ usable stack here. The values mirror AUDIO_ERR_* in include/audio.h.
    ldr   r1, =0x10103400         @ CSND channel 0
    mov   r0, #0
    strh  r0, [r1, #0x00]         @ stop the channel before reprogramming it
    ldr   r0, =0x80008000
    str   r0, [r1, #0x04]         @ volume: full left + right
    ldr   r0, =0x233D0000         @ AUDIO_ERR_ADDR
    str   r0, [r1, #0x0C]         @ sample source
    ldr   r0, =5200               @ AUDIO_ERR_BYTES (2600 samples, 16-bit)
    str   r0, [r1, #0x10]         @ length in bytes
    ldr   r0, =0xDF46             @ period reload for 8 kHz, two's complement
    strh  r0, [r1, #0x02]
    ldr   r0, =0xD840             @ start | enable | interp | PCM16 | one-shot
    strh  r0, [r1, #0x00]

.type crash_hang11, %function
crash_hang11:
    wfi
    b     crash_hang11
.pool

/* AUDIO_CMD_PARK: copied to AUDIO_PARK_ADDR and run there. It reports
 * AUDIO_ST_PARKED, then waits on the firm's mailbox as src/arm11_start.s does
 * at boot. Only PC-relative literals, so it runs from any address. */
.global core11_park_stub
.global core11_park_stub_end
.type core11_park_stub, %function
core11_park_stub:
    ldr   r0, .Lpark_ctrl
    mov   r1, #6                  @ AUDIO_ST_PARKED
    str   r1, [r0, #4]            @ AudioCtrl.status
    mov   r1, #0
    mcr   p15, 0, r1, c7, c10, 0  @ clean D-cache
    mcr   p15, 0, r1, c7, c10, 4  @ DSB
    ldr   r2, .Lpark_mailbox
.Lpark_wait:
    mcr   p15, 0, r2, c7, c6, 1   @ drop any cached copy of the mailbox
    ldr   r1, [r2]
    cmp   r1, #0
    beq   .Lpark_wait
    mov   r12, r1
    mov   r0, #0
    mcr   p15, 0, r0, c7, c14, 0  @ clean + invalidate D-cache
    mcr   p15, 0, r0, c7, c10, 4  @ DSB
    mcr   p15, 0, r0, c7, c5, 0   @ invalidate I-cache: the new core is new code
    mcr   p15, 0, r0, c7, c5, 6   @ flush branch predictor
    bx    r12
.Lpark_ctrl:
    .word 0x23300000              @ AUDIO_CTRL_ADDR
.Lpark_mailbox:
    .word 0x27000000              @ AUDIO_ARM11_MAILBOX
core11_park_stub_end:
