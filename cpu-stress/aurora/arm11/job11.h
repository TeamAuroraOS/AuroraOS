/* The ARM11 job: the workloads (../../common/work.c) built for the ARM11 and
 * run there by AuroraOS's core (AUDIO_CMD_RUN11, src/os/Run11.c). The app on
 * the ARM9 copies the code to JOB11_CODE and passes the block at JOB11_PARAMS;
 * both are in FCRAM an app may use and the SDK leaves alone here (the image
 * scratch, unused by this app). */
#ifndef JOB11_H
#define JOB11_H

#include <stdint.h>

#include "work.h"

#define JOB11_CODE   0x25100000u /* job11.ld links the code here */
#define JOB11_CODE_MAX 0x80000u
#define JOB11_PARAMS 0x25180000u

typedef struct {
  uint32_t kind, reps; /* work_run()'s */
  uint32_t sum;
  uint32_t ran;        /* set by the job, so a refused command shows */
  WorkCtx ctx;
} Job11;

#endif
