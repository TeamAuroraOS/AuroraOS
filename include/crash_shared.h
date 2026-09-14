/* Cross-core crash block: the ARM11 fault stub fills it and sets the magic
 * last; the ARM9 polls the magic and shows the crash screen.
 *
 * audio11_start.s hard-codes the offsets (magic 0, cpu 4, core 8, r[0] 12, pc
 * 64, cpsr 68, exc 72, dfsr 76, dfar 80). */
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
