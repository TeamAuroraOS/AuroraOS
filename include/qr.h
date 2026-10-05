#ifndef AURORA_QR_H
#define AURORA_QR_H

#include "aurora.h"

/* QR codes (ISO/IEC 18004) of up to version 10 at error correction level M:
 * byte mode, so any text up to 213 bytes. */

#define QR_MAX 57 /* modules across version 10 */

typedef struct {
  int size; /* modules across, 21 to QR_MAX; 0 when the text did not fit */
  u8 dark[QR_MAX * QR_MAX];
} Qr;

/* The smallest version that holds `len` bytes of `text`, with the mask that
 * scores best. Returns q->size. The quiet zone around it is the caller's. */
int qr_make(Qr *q, const char *text, u32 len);

static inline int qr_at(const Qr *q, int x, int y) {
  return q->dark[y * q->size + x];
}

#endif
