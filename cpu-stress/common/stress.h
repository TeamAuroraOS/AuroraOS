/* The CPU stress test, shared by the AuroraOS app (../aurora) and the
 * Horizon homebrew app (../horizon). stress.c runs the test: the phases, the
 * measurements, both screens and the results file. The workloads are in
 * work.c, and each platform runs them on the CPU under test: the ARM11 on
 * both. Each platform supplies the functions below. */
#ifndef STRESS_H
#define STRESS_H

#include <stdint.h>

#include "work.h"

/* Buttons, as plat_update() reports them. */
#define K_A     (1u << 0)
#define K_B     (1u << 1)
#define K_X     (1u << 2)
#define K_START (1u << 3)
#define K_L     (1u << 4)
#define K_R     (1u << 5)
#define K_LEFT  (1u << 6)
#define K_RIGHT (1u << 7)

#define TEMP_UNKNOWN (-1000)

typedef struct {
  int temp_c;   /* the battery's, or TEMP_UNKNOWN */
  int tenths;   /* charge in tenths of a percent, or -1 */
  int mv;       /* system voltage in millivolts, or -1 */
  int charging; /* 1, 0, or -1 when unknown */
} Battery;

typedef struct {
  const char *title;       /* "AuroraOS" or "Horizon", on the screens */
  const char *system;      /* the system and the API, for the results */
  const char *cpu;         /* the CPU the workloads run on, for the results */
  const char *cpu_short;   /* the same in a few words, for the screens */
  const char *build;       /* how the workloads were compiled */
  const char *model;       /* "New 3DS family", ... */
  const char *result_path; /* for fopen() */
  const char *result_name; /* as shown to the user */
} PlatInfo;

/* One more core running stress rounds (Horizon). */
typedef struct {
  volatile uint32_t rounds;
  volatile int stop;
  int core;
} StressWorker;

/* A free-running count, and its rate. */
uint64_t plat_ticks(void);
uint64_t plat_tick_hz(void);
/* Reads the buttons: those newly pressed go in *down. 0 means the system
 * wants the app to end. */
int plat_update(uint32_t *down);
/* The framebuffer to draw this update into: 3 bytes per pixel (B, G, R) by
 * column, pixel (x, y) at ((x * 240) + (239 - y)) * 3; 400 wide on top, 320
 * on the bottom. */
uint8_t *plat_fb(int bottom);
/* Shows both screens; may wait for the display. */
void plat_present(void);
/* Waits without using the CPU where the system allows. */
void plat_wait(uint32_t ms);
void plat_battery(Battery *b);
/* "2026-10-08 22:41", the console's clock. */
void plat_date(char *out, int size);
/* The settings that change the result ("804 MHz, L2 cache off"). */
void plat_notes(char *out, int size);
/* A setting X changes on the start screen, as a line to show, or 0. */
const char *plat_option(void);
void plat_option_toggle(void);

/* The context the workloads get; the test fills in its buffers. */
WorkCtx *plat_work_ctx(void);
/* Starts `reps` repetitions of workload `kind` on the CPU under test. The
 * screens may be drawn (not shown) until plat_work_wait(). */
void plat_work_start(uint32_t kind, uint32_t reps);
/* Waits for them: the ticks they took, or 0 if the CPU under test did not
 * answer. */
uint64_t plat_work_wait(void);

/* Starts stress_worker() on every other core the app may use, at most max;
 * fills w[i].core and returns how many run. */
int plat_cores_start(StressWorker *w, int max);
void plat_cores_stop(StressWorker *w, int n);

int stress_main(const PlatInfo *pi);
/* Stress rounds until w->stop, counting them in w->rounds. */
void stress_worker(StressWorker *w);

#endif
