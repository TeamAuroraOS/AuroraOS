/*
 * Cross-core crash hand-off (ARM9 <-> ARM11).
 *
 * The ARM11 audio core has no screen/power access, so when it faults its
 * exception stub fills this block in shared FCRAM and sets the magic; the ARM9
 * polls the magic (in get_keys_down) and shows the crash screen on its behalf.
 *
 * Field offsets are relied on by the ARM11 asm stubs in audio11_start.s -- keep
 * them in sync (magic 0, cpu 4, core 8, r[0] 12, pc 64, cpsr 68, exc 72,
 * dfsr 76, dfar 80).
 */
#ifndef AURORA_CRASH_SHARED_H
#define AURORA_CRASH_SHARED_H

#include <stdint.h>

#define CRASH_SHARED_ADDR  0x23380000u
#define CRASH_SHARED_MAGIC 0x48535243u 

enum { CRASH_CPU_ARM9 = 0, CRASH_CPU_ARM11 = 1 };

typedef struct {
  volatile uint32_t magic; 
  volatile uint32_t cpu;   
  volatile uint32_t core;  
  volatile uint32_t r[13];
  volatile uint32_t pc;
  volatile uint32_t cpsr;
  volatile uint32_t exc;
  volatile uint32_t dfsr;
  volatile uint32_t dfar;
} CrashShared;

#endif
