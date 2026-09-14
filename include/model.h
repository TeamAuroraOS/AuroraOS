#ifndef MODEL_H
#define MODEL_H

#include "aurora.h"

/* Model from CFG11_SOCINFO, which only the ARM11 can read. Until the ARM11 core
 * is up the model reads as Old. */

typedef enum {
  AURORA_MODEL_OLD = 0, /* 3DS, 3DS XL, 2DS      */
  AURORA_MODEL_NEW = 1, /* New 3DS, New 2DS XL   */
} AuroraModel;

AuroraModel aurora_model(void);
int aurora_is_new3ds(void);

/* One letter for the status bar: "N" on a New model, empty on an Old one. */
const char *aurora_model_tag(void);

u32 aurora_socinfo(void);

/* New 3DS clock switch to 804 MHz. It only applies once every ARM11 core waits
 * for an interrupt, so the outcome is read back rather than assumed. */
typedef enum {
  AURORA_N3DS_HW_OLD = 0, /* not a New 3DS: nothing to switch            */
  AURORA_N3DS_HW_APPLIED, /* the ARM11 is now in 804 MHz mode            */
  AURORA_N3DS_HW_ALREADY, /* it already was                              */
  AURORA_N3DS_HW_REFUSED, /* requested, but the mode did not change      */
  AURORA_N3DS_HW_NO_CORE, /* the ARM11 core did not answer               */
  AURORA_N3DS_HW_NO_IRQ,  /* no interrupt reached it, so it did not try  */
} AuroraN3dsHw;

AuroraN3dsHw aurora_n3ds_hardware(void);
const char *aurora_n3ds_hw_text(AuroraN3dsHw r);

/* The clock-mode register as the ARM11 read it before and after the last
 * aurora_n3ds_hardware() call, for diagnostics. */
void aurora_n3ds_clock(u32 *before, u32 *after);

#endif
