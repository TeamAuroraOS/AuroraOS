#ifndef FILES_H
#define FILES_H

#include "aurora.h"

/* Decided by the extension, and for .bin by the container magic. */
typedef enum {
  FKIND_DIR = 0,
  FKIND_AURORA, /* AOS1 / AUR1 app container, launchable          */
  FKIND_IMAGE,  /* png, jpg, jpeg, bmp                            */
  FKIND_AUDIO,  /* wav, mp3, aaf                                  */
  FKIND_BIN,    /* .bin that is not an Aurora container           */
  FKIND_TEXT,   /* txt, log                                       */
  FKIND_OTHER,
} FileKind;

/* `launch` receives a container path when an Aurora app is opened and is not
 * expected to return. 0 leaves app containers unopenable. */
void files_screen(void (*launch)(const char *path));

FileKind files_kind(const char *name, int is_dir);

#endif
