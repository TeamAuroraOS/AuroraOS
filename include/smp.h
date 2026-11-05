#ifndef AURORA_SMP_H
#define AURORA_SMP_H

#include <stdint.h>

/* The other ARM11 cores (AUDIO_CMD_SMP, Smp11.c; docs/audio.md "The other ARM11
 * cores"). Cores 1 to 3 (1 on an Old 3DS) wait in a small loop outside the core's
 * image, so a core 0 replaced by another build leaves them alone, and run jobs an
 * app left in FCRAM under a map of the app's choosing.
 *
 * Everything here is strongly ordered for the ARM11 side; the ARM9 cleans what
 * it writes and invalidates what it reads. */

#define AUDIO_SMP_VERSION 124 /* the first core that takes AUDIO_CMD_SMP */

#define SMP_STUB_ADDR  0x233E1000u /* the waiting loop, under 4 KB */
#define SMP_BLOCK_ADDR 0x233E2000u
#define SMP_TT_ADDR    0x233E4000u /* the jobs' first-level table, 16 KB */

#define SMP_CORES 4u
#define SMP_ALIVE 0x50494D53u /* 'SMIP', set by a core as it starts to wait */

/* In a job's FCRAM map. Anything not named stays strongly ordered. */
enum {
  SMP_MAP_CACHED = 1, /* write-back and coherent between the cores: the code,
                         heap and frames only the jobs touch */
  SMP_MAP_SHARED = 2, /* normal, not cached: what the ARM9, CSND or the GPU
                         also read or write */
};

/* AUDIO_CMD_SMP: arg0 an SMP_OP_*, arg1 as it says. All answer in SmpBlock. */
enum {
  SMP_OP_CORES = 1, /* arg1: mask of cores to have waiting, bit n = core n */
  SMP_OP_RUN,       /* arg1: an SmpJob's address, read once before the ack */
  SMP_OP_END,       /* sets stop, waits for the jobs to return (1 s at most) and
                       leaves nothing of theirs in any cache */
  SMP_OP_TEST,      /* the self-test: starts every core and measures them */
};

#define SMP_MAP_MAX 8u

typedef struct {
  uint32_t first_mb, mbs, attr;
} SmpMapEntry;

/* entry(arg, core) runs on each core in `cores`, with the MMU, caches, branch
 * prediction, VFP (RunFast) and unaligned access on, interrupts off. Returning
 * ends that core's part of the job. */
typedef struct {
  uint32_t entry;
  uint32_t arg;
  uint32_t cores;
  uint32_t sp[SMP_CORES];
  uint32_t map_count;
  SmpMapEntry map[SMP_MAP_MAX];
} SmpJob;

/* The offsets of the first five are in audio11_start.s. */
typedef struct {
  volatile uint32_t alive[SMP_CORES];   /* 0x00: SMP_ALIVE while waiting */
  volatile uint32_t entry[SMP_CORES];   /* 0x10: taken as the job starts */
  volatile uint32_t arg[SMP_CORES];     /* 0x20 */
  volatile uint32_t sp[SMP_CORES];      /* 0x30 */
  volatile uint32_t running[SMP_CORES]; /* 0x40: 1 while in a job */
  volatile uint32_t started;            /* mask of the cores that answered */
  volatile uint32_t stop;               /* jobs look at this and return */
  volatile uint32_t failed;             /* mask of those asked for that did not */
  volatile uint32_t test_runs;          /* SMP_OP_TEST: finished runs */
  volatile uint32_t test_sum;           /* the shared counter's end value */
  volatile uint32_t test_want;          /* what it should be */
  volatile uint32_t test_cpu[SMP_CORES]; /* timer ticks for the loop */
  volatile uint32_t test_mem[SMP_CORES]; /* timer ticks for the memory pass */
  volatile uint32_t test_done[SMP_CORES];
} SmpBlock;

/* The self-test's sizes, for working out its results. Timer ticks are half a
 * CPU clock. */
#define SMP_TEST_LOOPS 4000000u
#define SMP_TEST_BYTES (256u * 1024u)
#define SMP_TEST_PASSES 4u

#endif
