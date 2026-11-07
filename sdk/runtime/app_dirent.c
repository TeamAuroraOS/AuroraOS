/* <dirent.h> over fs_dir_open(). devkitARM's header is libsysbase's, whose DIR
 * reaches the device's state through a DIR_ITER; libsysbase is not linked, so
 * this fills that DIR in itself, with the FsDir as the DIR_ITER's state. */
#include "app_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  DIR d;
  DIR_ITER it;
} DirBox;

static FsDir *fs_of(DIR *d) { return (FsDir *)d->dirData->dirStruct; }

DIR *opendir(const char *path) {
  struct stat st;
  DirBox *b;
  FsDir *f;
  int e;

  if (!path || !*path) {
    errno = ENOENT;
    return NULL;
  }
  if ((e = app_fs_stat(path, &st)) < 0) {
    errno = -e;
    return NULL;
  }
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return NULL;
  }
  b = calloc(1, sizeof(*b));
  if (!b) {
    errno = ENOMEM;
    return NULL;
  }
  f = fs_dir_open(path);
  if (!f) {
    free(b);
    errno = EIO;
    return NULL;
  }
  b->it.dirStruct = f;
  b->d.dirData = &b->it;
  return &b->d;
}

struct dirent *readdir(DIR *d) {
  FsEntry e;
  if (!d) {
    errno = EBADF;
    return NULL;
  }
  if (!fs_dir_read(fs_of(d), &e))
    return NULL;
  d->position++;
  /* Some programs skip entries whose d_ino is 0. */
  d->fileData.d_ino = (ino_t)d->position;
  d->fileData.d_type = e.is_dir ? DT_DIR : DT_REG;
  strncpy(d->fileData.d_name, e.name, NAME_MAX);
  d->fileData.d_name[NAME_MAX] = '\0';
  return &d->fileData;
}

int readdir_r(DIR *d, struct dirent *entry, struct dirent **result) {
  struct dirent *r;
  int saved = errno;
  if (!d || !entry || !result)
    return EBADF;
  errno = 0;
  r = readdir(d);
  if (!r && errno) {
    *result = NULL;
    return errno;
  }
  errno = saved;
  if (r)
    *entry = *r;
  *result = r ? entry : NULL;
  return 0;
}

int closedir(DIR *d) {
  if (!d) {
    errno = EBADF;
    return -1;
  }
  fs_dir_close(fs_of(d));
  free(d); /* the DIR is the box's first member */
  return 0;
}

void rewinddir(DIR *d) {
  if (!d)
    return;
  app_fs_dir_rewind(fs_of(d));
  d->position = 0;
}

long int telldir(DIR *d) { return d ? d->position : -1; }

void seekdir(DIR *d, long int loc) {
  if (!d || loc < 0)
    return;
  rewinddir(d);
  while (d->position < loc && readdir(d))
    ;
}

int alphasort(const struct dirent **a, const struct dirent **b) {
  return strcoll((*a)->d_name, (*b)->d_name);
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
  DIR *d = opendir(dirp);
  struct dirent **list = NULL, *e;
  int n = 0, cap = 0;

  if (!d)
    return -1;
  while ((e = readdir(d)) != NULL) {
    struct dirent *copy;
    if (filter && !filter(e))
      continue;
    if (n == cap) {
      struct dirent **grown;
      cap = cap ? cap * 2 : 16;
      grown = realloc(list, (size_t)cap * sizeof(*list));
      if (!grown)
        goto nomem;
      list = grown;
    }
    copy = malloc(sizeof(*copy));
    if (!copy)
      goto nomem;
    *copy = *e;
    list[n++] = copy;
  }
  closedir(d);
  if (compar && n > 1)
    qsort(list, (size_t)n, sizeof(*list),
          (int (*)(const void *, const void *))compar);
  *namelist = list;
  return n;

nomem:
  while (n)
    free(list[--n]);
  free(list);
  closedir(d);
  errno = ENOMEM;
  return -1;
}
