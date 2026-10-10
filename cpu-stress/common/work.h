/* The workloads, in one file that builds anywhere: no C library, no state but
 * what the caller passes. Horizon runs them in the app; AuroraOS runs them on
 * the ARM11 as a job (../aurora/arm11). */
#ifndef STRESS_WORK_H
#define STRESS_WORK_H

#include <stdint.h>

/* What one repetition of each workload does. */
#define CLOCK_PASSES 1000u        /* x 256 dependent adds */
#define INT_CHUNK    50000u       /* iterations */
#define FLOAT_CHUNK  2000u        /* iterations, 8 operations each */
#define MEM_BYTES    (4u << 20)   /* past every cache, the New 3DS's 2 MB L2 too */
#define ROUND_INT    20000u
#define ROUND_FLOAT  4000u
#define ROUND_MEM    (64u * 1024u)

enum {
  W_CLOCK, /* 256 dependent adds a pass: one cycle each */
  W_INT,
  W_FLOAT,
  W_READ,  /* MEM_BYTES from big_a */
  W_WRITE, /* MEM_BYTES to big_b */
  W_COPY,  /* big_a to big_b */
  W_ROUND, /* one stress round: integer, float, a ROUND_MEM copy */
  W_COUNT
};

/* Shared with the ARM11 job as it is: pointers are 32 bits on both CPUs. */
typedef struct {
  uint32_t *big_a, *big_b; /* MEM_BYTES each */
  uint32_t *ra, *rb;       /* ROUND_MEM each */
  uint32_t seed;
  uint32_t sink;           /* keeps the results alive */
} WorkCtx;

/* `reps` repetitions of `kind`; returns a checksum. */
uint32_t work_run(WorkCtx *c, uint32_t kind, uint32_t reps);

#endif
