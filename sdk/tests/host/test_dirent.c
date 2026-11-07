/* <dirent.h> (sdk/runtime/app_dirent.c) over the real app_fs.c and FatFs, on
 * a RAM disk. */
#include "app_internal.h"
#include "check.h"
#include "hostrt.h"
#include "model.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

static void put(const char *path, const char *text) {
  int fd = app_fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
  CHECK(fd >= APP_FD_FIRST);
  CHECK_EQ(app_fs_write(fd, text, strlen(text)), (int)strlen(text));
  CHECK_EQ(app_fs_close(fd), 0);
}

static void disk(void) {
  model_reset(125);
  host_ramdisk(64);
  CHECK(fs_mkdir("/Aurora/Apps/C/Test/levels"));
  put("/Aurora/Apps/C/Test/b.txt", "bb");
  put("/Aurora/Apps/C/Test/a.txt", "a");
  put("/Aurora/Apps/C/Test/A long file name.dat", "long");
  put("/Aurora/Apps/C/Test/levels/1.lvl", "1");
}

static int count(DIR *d, int *dirs) {
  struct dirent *e;
  int n = 0;
  *dirs = 0;
  while ((e = readdir(d)) != NULL) {
    n++;
    CHECK(e->d_ino != 0);
    CHECK(e->d_type == DT_DIR || e->d_type == DT_REG);
    CHECK(strcmp(e->d_name, ".") && strcmp(e->d_name, ".."));
    if (e->d_type == DT_DIR)
      (*dirs)++;
    else
      CHECK(strcmp(e->d_name, "levels") && strcmp(e->d_name, "Aurora"));
  }
  return n;
}

static void t_read(void) {
  DIR *d;
  int dirs;
  disk();
  d = opendir("/Aurora/Apps/C/Test");
  CHECK(d != NULL);
  CHECK_EQ(count(d, &dirs), 4);
  CHECK_EQ(dirs, 1);
  CHECK(readdir(d) == NULL);
  CHECK_EQ(telldir(d), 4);
  rewinddir(d);
  CHECK_EQ(telldir(d), 0);
  CHECK_EQ(count(d, &dirs), 4);
  CHECK_EQ(closedir(d), 0);

  /* The SDK's other spellings of a path, and the working directory. */
  d = opendir("sdmc:/Aurora/Apps/C/Test/levels");
  CHECK(d != NULL);
  struct dirent *e = readdir(d);
  CHECK(e && !strcmp(e->d_name, "1.lvl") && e->d_type == DT_REG);
  closedir(d);
  CHECK_EQ(app_fs_chdir("/Aurora/Apps/C"), 0);
  d = opendir("Test/../Test/levels");
  CHECK(d && readdir(d) && !readdir(d));
  closedir(d);
  d = opendir(".");
  CHECK(d && readdir(d) && !readdir(d)); /* just "Test" */
  closedir(d);
  d = opendir("/");
  CHECK(d && count(d, &dirs) == 1 && dirs == 1); /* "Aurora" */
  closedir(d);
}

static void t_seek(void) {
  char third[NAME_MAX + 1];
  struct dirent *e;
  DIR *d;
  disk();
  d = opendir("/Aurora/Apps/C/Test");
  readdir(d);
  readdir(d);
  long pos = telldir(d);
  e = readdir(d);
  CHECK(e != NULL);
  strcpy(third, e->d_name);
  readdir(d);
  seekdir(d, pos);
  e = readdir(d);
  CHECK(e && !strcmp(e->d_name, third));
  struct dirent ent, *res = NULL;
  rewinddir(d);
  CHECK_EQ(readdir_r(d, &ent, &res), 0);
  CHECK(res == &ent && ent.d_name[0]);
  for (int i = 0; i < 3; i++)
    readdir_r(d, &ent, &res);
  CHECK_EQ(readdir_r(d, &ent, &res), 0);
  CHECK(res == NULL);
  closedir(d);
}

static int no_dirs(const struct dirent *e) { return e->d_type != DT_DIR; }

static void t_scandir(void) {
  struct dirent **list;
  int n;
  disk();
  n = scandir("/Aurora/Apps/C/Test", &list, NULL, alphasort);
  CHECK_EQ(n, 4);
  if (n == 4) {
    CHECK(!strcmp(list[0]->d_name, "A long file name.dat"));
    CHECK(!strcmp(list[1]->d_name, "a.txt"));
    CHECK(!strcmp(list[2]->d_name, "b.txt"));
    CHECK(!strcmp(list[3]->d_name, "levels"));
  }
  while (n > 0)
    free(list[--n]);
  free(list);
  n = scandir("/Aurora/Apps/C/Test", &list, no_dirs, alphasort);
  CHECK_EQ(n, 3);
  while (n > 0)
    free(list[--n]);
  free(list);
  n = scandir("/Aurora/Apps/C/Test/levels", &list, NULL, NULL);
  CHECK_EQ(n, 1);
  free(list[0]);
  free(list);
  errno = 0;
  CHECK_EQ(scandir("/nothing", &list, NULL, NULL), -1);
  CHECK_EQ(errno, ENOENT);
}

static void t_errors(void) {
  disk();
  errno = 0;
  CHECK(opendir("/Aurora/none") == NULL && errno == ENOENT);
  CHECK(opendir("/Aurora/Apps/C/Test/a.txt") == NULL && errno == ENOTDIR);
  CHECK(opendir("") == NULL && errno == ENOENT);
  CHECK(opendir(NULL) == NULL && errno == ENOENT);
  CHECK(readdir(NULL) == NULL && errno == EBADF);
  CHECK(closedir(NULL) == -1 && errno == EBADF);
  /* Removing an entry while listing does not upset the listing. */
  DIR *d = opendir("/Aurora/Apps/C/Test");
  int dirs;
  readdir(d);
  CHECK_EQ(app_fs_unlink("/Aurora/Apps/C/Test/b.txt"), 0);
  CHECK(count(d, &dirs) <= 3);
  closedir(d);
}

SCENARIOS("dirent", S(t_read), S(t_seek), S(t_scandir), S(t_errors))
