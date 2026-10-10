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
  u32 dfsr;  /* Data Abort only */
  u32 dfar;
  u32 cpu;   /* CRASH_CPU_* (crash_shared.h)  */
  u32 core;  /* ARM11 core id, else 0         */
} CrashDump;

/* Call once at OS startup. */
void crash_init(void);

/* Set by a native app's runtime (sdk/runtime): polled through the power-off
 * countdown, it does not return once the user asks to go back to the Home
 * Menu, and the countdown then says HOME does that. The OS leaves it unset. */
extern void (*g_crash_poll)(void);

/* Never returns. */
void crash_handle(CrashDump *d);

/* A user-forced crash of the current state; never returns. */
void crash_force(void);

void crash_capture(CrashDump *d);

/* Shows the crash screen if the ARM11 posted a fault. Cheap enough to call from
 * the input poll. */
void crash_poll_arm11(void);

#endif
