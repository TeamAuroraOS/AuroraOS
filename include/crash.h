/*
 * AuroraOS custom crash handler (ARM9).
 *
 * Installs exception vectors so an Undefined-Instruction / Prefetch-Abort /
 * Data-Abort on the ARM9 (where the OS runs) is caught and shown on a blue
 * crash screen -- a sad face on the top screen, the register/reason dump on the
 * bottom -- instead of silently hanging. The Settings menu can also force one
 * ("User Forced Crash") to test it.
 */
#ifndef AURORA_CRASH_H
#define AURORA_CRASH_H

#include "aurora.h"

/* Exception / reason codes (also the index into the reason strings). */
enum {
  CRASH_UNDEF = 0, /* Undefined Instruction */
  CRASH_PABT  = 1, /* Prefetch Abort        */
  CRASH_DABT  = 2, /* Data Abort            */
  CRASH_USER  = 3, /* User Forced Crash     */
};

/* CPU snapshot captured at the fault. Layout is shared with crash.s -- keep the
 * field order and offsets in sync (r[0] at 0, pc at 52, cpsr 56, exc 60,
 * dfsr 64, dfar 68). */
typedef struct {
  u32 r[13]; /* r0-r12 at the fault           */
  u32 pc;    /* faulting instruction address  */
  u32 cpsr;  /* mode/flags at the fault       */
  u32 exc;   /* CRASH_*                       */
  u32 dfsr;  /* data fault status (Data Abort)*/
  u32 dfar;  /* data fault address (Data Abort)*/
  u32 cpu;   /* CRASH_CPU_* (crash_shared.h)  */
  u32 core;  /* ARM11 core id, else 0         */
} CrashDump;

/* Install the exception vectors. Call once at OS startup. */
void crash_init(void);

/* Show the crash screen for `d` and hang forever. Never returns. */
void crash_handle(CrashDump *d);

/* Trigger a "User Forced Crash" (captures the current CPU state). Never
 * returns. Called by the Settings "Force Debug Crash" item. */
void crash_force(void);

/* asm (crash.s): fill `d` with the current register state. */
void crash_capture(CrashDump *d);

/* Check the cross-core crash block for an ARM11 fault; if one is posted, show
 * the crash screen for it (never returns). Cheap fast-path; call it from the
 * input poll (get_keys_down). */
void crash_poll_arm11(void);

#endif /* AURORA_CRASH_H */
