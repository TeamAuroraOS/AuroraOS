#ifndef WAV_H
#define WAV_H

#include "aurora.h"

/* RIFF/WAVE reader: uncompressed PCM, converted to signed mono for CSND. */

typedef enum {
  WAV_OK = 0,
  WAV_ERR_OPEN,
  WAV_ERR_FORMAT,      /* not a RIFF/WAVE file                 */
  WAV_ERR_UNSUPPORTED, /* compressed, or a depth other than 8/16 */
  WAV_ERR_DATA,
} WavResult;

/* Loads `path` into the OS PCM buffer and starts playback on channel 0. */
WavResult wav_play(const char *path, u32 *samples, u32 *rate, u32 *depth);

typedef struct {
  u32 samples; /* mono samples written          */
  u32 rate;    /* their rate, after any halving */
  u32 depth;   /* 8 or 16                       */
} WavInfo;

/* Converts `path` into `dst` as signed mono at the file's depth, up to `max`
 * bytes. With `halve`, a file above 32 kHz loads at half its rate by averaging
 * sample pairs. */
WavResult wav_load(const char *path, u8 *dst, u32 max, int halve,
                   WavInfo *info);

const char *wav_error(WavResult r);

#endif
