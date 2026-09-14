#ifndef IMAGE_H
#define IMAGE_H

#include "aurora.h"

/* Still images for the File Explorer (BMP, PNG, baseline JPEG), decoded to
 * RGB888. */

/* Working buffers, live only while the viewer is open. Clear of the return
 * descriptor (0x25008000), the OS snapshot (0x26000000) and the ARM11 mailbox
 * (0x27000000). */
#define IMAGE_FILE_ADDR 0x25100000u /* file bytes as read from the card  */
#define IMAGE_FILE_MAX  (6u * 1024u * 1024u)
#define IMAGE_RAW_ADDR  0x25700000u /* decoder scratch (PNG scanlines)   */
#define IMAGE_RAW_MAX   (8u * 1024u * 1024u)
#define IMAGE_RGB_ADDR  0x26100000u /* decoded RGB888                    */
#define IMAGE_RGB_MAX   (8u * 1024u * 1024u)

#define IMAGE_MAX_PIXELS (IMAGE_RGB_MAX / 3u)
#define IMAGE_MAX_DIM    8192 /* guards width*height before it is computed */

typedef enum {
  IMG_OK = 0,
  IMG_ERR_OPEN,
  IMG_ERR_TOO_BIG,     /* file, or decoded size, exceeds a cap   */
  IMG_ERR_FORMAT,
  IMG_ERR_UNSUPPORTED, /* a variant of the format that is not done */
  IMG_ERR_DATA,        /* truncated or corrupt                   */
} ImageResult;

typedef struct {
  int w, h;
  const u8 *rgb; /* w*h*3, R,G,B order */
} Image;

/* The card must already be mounted. */
ImageResult image_load(const char *path, Image *out);

const char *image_error(ImageResult r);

/* Draws `img` centred and scaled down to fit by area averaging; never enlarged. */
void image_draw_fit(volatile u8 *fb, int bx, int by, int bw, int bh,
                    int screen_height, const Image *img);

/* Decodes into `rgb` and reports the size through `w`/`h`. */
ImageResult jpeg_decode(const u8 *data, u32 len, u8 *rgb, u32 rgb_max, int *w,
                        int *h);

#endif
