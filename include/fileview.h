#ifndef FILEVIEW_H
#define FILEVIEW_H

#include "aurora.h"

/* `path` is a FatFs path on a mounted card. Each viewer returns with both
 * screens left for the caller to redraw. */

void text_view(const char *path, const char *name, u32 size);

void hex_edit(const char *path, const char *name, u32 size);

/* A or the left button answers yes (1), B or the right button no (0). */
int fv_confirm(const char *title, const char *subtitle, const char *yes,
               const char *no);

#endif
