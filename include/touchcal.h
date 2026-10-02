#ifndef AURORA_TOUCHCAL_H
#define AURORA_TOUCHCAL_H

#include "touch.h"

/* Returns 1 with the new calibration applied and in `result`, for the caller to
 * save; 0 when cancelled, with the old one still in use. Leaves both screens
 * for the caller to redraw. */
int touch_calibrate(TouchCal *result);

#endif
