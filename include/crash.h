/* ARM9 crash handler: exception vectors catch undefined instructions, prefetch
 * aborts and data aborts and show the crash screen. */
#ifndef AURORA_CRASH_H
#define AURORA_CRASH_H

#include "aurora.h"

/* Also the index into the reason strings. */
enum {
  CRASH_UNDEF = 0,
  CRASH_PABT  = 1,
  CRASH_DABT  = 2,
  CRASH_USER  = 3,
};

/* CPU snapshot captured at the fault. Layout is shared with crash.s, keep the
 * field order and offsets in sync (r[0] at 0, pc at 52, cpsr 56, exc 60,
 * dfsr 64, dfar 68). */
typedef struct {
  u32 r[13];
  u32 pc;
  u32 cpsr;
  u32 exc;   /* CRASH_*                       */
  u32 dfsr;  /* data fault status (Data Abort)*/
  u32 dfar;  /* data fault address (Data Abort)*/
  u32 cpu;   /* CRASH_CPU_* (crash_shared.h)  */
  u32 core;  /* ARM11 core id, else 0         */
} CrashDump;

/* Call once at OS startup. */
void crash_init(void);

/* Shows the crash screen for `d`; never returns. */
void crash_handle(CrashDump *d);

/* Captures the current CPU state as a user-forced crash; never returns. */
void crash_force(void);

/* asm (crash.s): fill `d` with the current register state. */
void crash_capture(CrashDump *d);

/* Shows the crash screen if the ARM11 posted a fault. Cheap enough to call from
 * the input poll. */
void crash_poll_arm11(void);

#endif
