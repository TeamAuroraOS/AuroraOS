/* Files on the SD card through FatFs: the fs_ functions, and the descriptor
 * table behind the C library's open(), read(), write() and the rest. */
#include "app_internal.h"
#include "ff.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_MAX_FS (FS_NAME_MAX + 8)

static FATFS g_fs;
static bool g_mounted;
/* The working directory, as a FatFs path without the final "/": "0:" is the
 * root. */
static char g_cwd[FS_NAME_MAX + 8] = "0:";

typedef struct {
  FIL f;
  bool used, append;
} FdSlot;

static FdSlot g_fd[APP_FD_COUNT];

struct FsDir {
  DIR d;
};

bool app_fs_mount(void) {
  if (g_mounted)
    return true;
  if (f_mount(&g_fs, "0:", 1) == FR_OK)
    return g_mounted = true;
  f_mount(NULL, "0:", 0);
  return false;
}

bool fs_available(void) { return app_fs_mount(); }

bool app_fs_path(const char *in, char *out, size_t max) {
  const char *p = in;
  size_t n = 2;

  if (!in || max < 4)
    return false;
  if (!strncmp(p, "sdmc:", 5))
    p += 5;
  else if (!strncmp(p, "sd:", 3))
    p += 3;
  else if (p[0] == '0' && p[1] == ':')
    p += 2;
  else if (*p != '/' && *p != '\\') {
    n = strlen(g_cwd);
    if (n >= max)
      return false;
  }
  if (n > 2)
    memcpy(out, g_cwd, n);
  out[0] = '0';
  out[1] = ':';
  while (*p) {
    const char *e;
    size_t len;
    while (*p == '/' || *p == '\\')
      p++;
    if (!*p)
      break;
    for (e = p; *e && *e != '/' && *e != '\\'; e++)
      ;
    len = (size_t)(e - p);
    if (len == 2 && p[0] == '.' && p[1] == '.') {
      while (n > 2 && out[n - 1] != '/')
        n--;
      if (n > 2)
        n--;
    } else if (!(len == 1 && p[0] == '.')) {
      if (n + 1 + len >= max)
        return false;
      out[n++] = '/';
      memcpy(out + n, p, len);
      n += len;
    }
    p = e;
  }
  if (n == 2)
    out[n++] = '/';
  out[n] = '\0';
  return true;
}

static bool is_root(const char *fp) { return fp[2] == '/' && !fp[3]; }

static int fr_errno(FRESULT r) {
  switch (r) {
  case FR_OK:
    return 0;
  case FR_NO_FILE:
  case FR_NO_PATH:
    return ENOENT;
  case FR_INVALID_NAME:
  case FR_INVALID_PARAMETER:
    return EINVAL;
  case FR_DENIED:
    return EACCES;
  case FR_EXIST:
    return EEXIST;
  case FR_INVALID_OBJECT:
    return EBADF;
  case FR_WRITE_PROTECTED:
    return EROFS;
  case FR_INVALID_DRIVE:
  case FR_NOT_ENABLED:
  case FR_NO_FILESYSTEM:
  case FR_NOT_READY:
    return ENODEV;
  case FR_TOO_MANY_OPEN_FILES:
    return EMFILE;
  case FR_NOT_ENOUGH_CORE:
    return ENOMEM;
  case FR_LOCKED:
  case FR_TIMEOUT:
    return EBUSY;
  default:
    return EIO;
  }
}

/* Mounts and converts the path; 0 or a negative errno. */
static int prepare(const char *path, char *fp) {
  if (!app_fs_mount())
    return -ENODEV;
  if (!app_fs_path(path, fp, PATH_MAX_FS))
    return -ENAMETOOLONG;
  return 0;
}

static FdSlot *slot(int fd) {
  fd -= APP_FD_FIRST;
  return fd >= 0 && fd < APP_FD_COUNT && g_fd[fd].used ? &g_fd[fd] : NULL;
}

int app_fs_open(const char *path, int flags) {
  char fp[PATH_MAX_FS];
  BYTE mode;
  FRESULT r;
  int i, e = prepare(path, fp);

  if (e)
    return e;
  for (i = 0; i < APP_FD_COUNT && g_fd[i].used; i++)
    ;
  if (i == APP_FD_COUNT)
    return -EMFILE;
  switch (flags & O_ACCMODE) {
  case O_RDONLY:
    mode = FA_READ;
    break;
  case O_WRONLY:
    mode = FA_WRITE;
    break;
  default:
    mode = FA_READ | FA_WRITE;
    break;
  }
  if ((flags & O_CREAT) && (flags & O_EXCL))
    mode |= FA_CREATE_NEW;
  else if ((flags & O_CREAT) && (flags & O_TRUNC))
    mode |= FA_CREATE_ALWAYS;
  else if (flags & O_CREAT)
    mode |= FA_OPEN_ALWAYS;
  else
    mode |= FA_OPEN_EXISTING;
  r = f_open(&g_fd[i].f, fp, mode);
  if (r != FR_OK)
    return -fr_errno(r);
  if ((flags & O_TRUNC) && !(flags & O_CREAT) && (mode & FA_WRITE))
    f_truncate(&g_fd[i].f);
  g_fd[i].used = true;
  g_fd[i].append = (flags & O_APPEND) != 0;
  return APP_FD_FIRST + i;
}

int app_fs_close(int fd) {
  FdSlot *s = slot(fd);
  FRESULT r;
  if (!s)
    return -EBADF;
  r = f_close(&s->f);
  s->used = false;
  return -fr_errno(r);
}

void app_fs_close_all(void) {
  for (int i = 0; i < APP_FD_COUNT; i++)
    if (g_fd[i].used) {
      f_close(&g_fd[i].f);
      g_fd[i].used = false;
    }
}

int app_fs_read(int fd, void *buf, size_t n) {
  FdSlot *s = slot(fd);
  UINT got = 0;
  FRESULT r;
  if (!s)
    return -EBADF;
  r = f_read(&s->f, buf, (UINT)n, &got);
  return r == FR_OK ? (int)got : -fr_errno(r);
}

int app_fs_write(int fd, const void *buf, size_t n) {
  FdSlot *s = slot(fd);
  UINT put = 0;
  FRESULT r;
  if (!s)
    return -EBADF;
  if (s->append)
    f_lseek(&s->f, f_size(&s->f));
  r = f_write(&s->f, buf, (UINT)n, &put);
  if (r != FR_OK)
    return r == FR_DENIED ? -EBADF : -fr_errno(r);
  return put || !n ? (int)put : -ENOSPC;
}

long app_fs_lseek(int fd, long off, int whence) {
  FdSlot *s = slot(fd);
  long long pos;
  FRESULT r;
  if (!s)
    return -EBADF;
  pos = whence == SEEK_CUR ? (long long)f_tell(&s->f)
        : whence == SEEK_END ? (long long)f_size(&s->f)
                             : 0;
  pos += off;
  if (pos < 0 || pos > 0xFFFFFFFFLL)
    return -EINVAL;
  r = f_lseek(&s->f, (FSIZE_t)pos);
  return r == FR_OK ? (long)f_tell(&s->f) : -fr_errno(r);
}

/* FatFs stamps dates in local time; time() counts from the console's local
 * clock too, so the two agree. */
static long long fat_time(WORD date, WORD time) {
  return app_civil_secs(1980 + (date >> 9), (date >> 5) & 15, date & 31,
                        time >> 11, (time >> 5) & 63, (time & 31) * 2);
}

int app_fs_fstat(int fd, void *out) {
  struct stat *st = out;
  FdSlot *s = slot(fd);
  if (!s)
    return -EBADF;
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG | 0666;
  st->st_nlink = 1;
  st->st_size = (off_t)f_size(&s->f);
  st->st_blksize = 512;
  return 0;
}

int app_fs_stat(const char *path, void *out) {
  struct stat *st = out;
  char fp[PATH_MAX_FS];
  FILINFO fno;
  FRESULT r;
  int e = prepare(path, fp);

  if (e)
    return e;
  memset(st, 0, sizeof(*st));
  st->st_nlink = 1;
  st->st_blksize = 512;
  if (is_root(fp)) {
    st->st_mode = S_IFDIR | 0777;
    return 0;
  }
  r = f_stat(fp, &fno);
  if (r != FR_OK)
    return -fr_errno(r);
  st->st_mode = (fno.fattrib & AM_DIR) ? (S_IFDIR | 0777) : (S_IFREG | 0666);
  st->st_size = (off_t)fno.fsize;
  st->st_mtime = st->st_atime = st->st_ctime =
      (time_t)fat_time(fno.fdate, fno.ftime);
  return 0;
}

int app_fs_unlink(const char *path) {
  char fp[PATH_MAX_FS];
  FILINFO fno;
  FRESULT r;
  int e = prepare(path, fp);
  if (e)
    return e;
  r = f_stat(fp, &fno);
  if (r != FR_OK)
    return -fr_errno(r);
  if (fno.fattrib & AM_DIR)
    return -EISDIR;
  return -fr_errno(f_unlink(fp));
}

int app_fs_rmdir(const char *path) {
  char fp[PATH_MAX_FS];
  FILINFO fno;
  FRESULT r;
  int e = prepare(path, fp);
  if (e)
    return e;
  if (is_root(fp))
    return -EBUSY;
  r = f_stat(fp, &fno);
  if (r != FR_OK)
    return -fr_errno(r);
  if (!(fno.fattrib & AM_DIR))
    return -ENOTDIR;
  r = f_unlink(fp);
  return r == FR_DENIED ? -ENOTEMPTY : -fr_errno(r);
}

/* FatFs refuses a name that exists; rename() replaces a file there. */
int app_fs_rename(const char *from, const char *to) {
  char ff[PATH_MAX_FS], ft[PATH_MAX_FS];
  FILINFO fno;
  int e = prepare(from, ff);
  if (e || (e = prepare(to, ft)))
    return e;
  if (!strcmp(ff, ft))
    return 0;
  if (f_stat(ft, &fno) == FR_OK && !(fno.fattrib & AM_DIR))
    f_unlink(ft);
  return -fr_errno(f_rename(ff, ft));
}

int app_fs_ftruncate(int fd, long len) {
  FdSlot *s = slot(fd);
  FSIZE_t at;
  FRESULT r;
  if (!s)
    return -EBADF;
  if (len < 0)
    return -EINVAL;
  at = f_tell(&s->f);
  /* FatFs cuts at the file pointer, and a seek past the end in write mode
   * makes the file longer, so a pointer past a new, shorter end moves back to
   * it rather than growing the file again. */
  r = f_lseek(&s->f, (FSIZE_t)len);
  if (r == FR_OK && (FSIZE_t)len < f_size(&s->f))
    r = f_truncate(&s->f);
  f_lseek(&s->f, at < (FSIZE_t)len ? at : (FSIZE_t)len);
  return r == FR_DENIED ? -EINVAL : -fr_errno(r);
}

int app_fs_fsync(int fd) {
  FdSlot *s = slot(fd);
  return s ? -fr_errno(f_sync(&s->f)) : -EBADF;
}

int app_fs_chdir(const char *path) {
  char fp[PATH_MAX_FS];
  FILINFO fno;
  int e = prepare(path, fp);
  if (e)
    return e;
  if (!is_root(fp)) {
    FRESULT r = f_stat(fp, &fno);
    if (r != FR_OK)
      return -fr_errno(r);
    if (!(fno.fattrib & AM_DIR))
      return -ENOTDIR;
  }
  strcpy(g_cwd, fp);
  if (is_root(g_cwd))
    g_cwd[2] = '\0';
  return 0;
}

const char *app_fs_getcwd(void) { return g_cwd[2] ? g_cwd + 2 : "/"; }

int app_fs_mkdir(const char *path) {
  char fp[PATH_MAX_FS];
  int e = prepare(path, fp);
  if (e)
    return e;
  if (is_root(fp))
    return -EEXIST;
  return -fr_errno(f_mkdir(fp));
}

FsDir *fs_dir_open(const char *path) {
  char fp[PATH_MAX_FS];
  FsDir *d;
  if (prepare(path, fp))
    return NULL;
  d = malloc(sizeof(*d));
  if (d && f_opendir(&d->d, fp) != FR_OK) {
    free(d);
    d = NULL;
  }
  return d;
}

bool fs_dir_read(FsDir *dir, FsEntry *entry) {
  FILINFO fno;
  if (!dir || !entry)
    return false;
  for (;;) {
    if (f_readdir(&dir->d, &fno) != FR_OK || !fno.fname[0])
      return false;
    if (strcmp(fno.fname, ".") && strcmp(fno.fname, ".."))
      break;
  }
  strncpy(entry->name, fno.fname, FS_NAME_MAX - 1);
  entry->name[FS_NAME_MAX - 1] = '\0';
  entry->is_dir = (fno.fattrib & AM_DIR) != 0;
  entry->size = entry->is_dir ? 0 : (uint32_t)fno.fsize;
  return true;
}

void fs_dir_close(FsDir *dir) {
  if (dir) {
    f_closedir(&dir->d);
    free(dir);
  }
}

void *fs_read_file(const char *path, size_t *size) {
  static FIL f;
  char fp[PATH_MAX_FS];
  u8 *buf;
  UINT got = 0;
  u32 len;

  if (size)
    *size = 0;
  if (prepare(path, fp) || f_open(&f, fp, FA_READ) != FR_OK)
    return NULL;
  len = (u32)f_size(&f);
  buf = malloc((size_t)len + 1);
  if (buf && (f_read(&f, buf, len, &got) != FR_OK || got != len)) {
    free(buf);
    buf = NULL;
  }
  f_close(&f);
  if (!buf)
    return NULL;
  buf[len] = 0;
  if (size)
    *size = len;
  return buf;
}

bool fs_write_file(const char *path, const void *data, size_t size) {
  static FIL f;
  char fp[PATH_MAX_FS];
  UINT put = 0;
  bool ok;

  if (prepare(path, fp) || f_open(&f, fp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    return false;
  ok = f_write(&f, data, (UINT)size, &put) == FR_OK && put == size;
  return f_close(&f) == FR_OK && ok;
}

static bool stat_of(const char *path, FILINFO *fno, bool *root) {
  char fp[PATH_MAX_FS];
  if (prepare(path, fp))
    return false;
  *root = is_root(fp);
  return *root || f_stat(fp, fno) == FR_OK;
}

bool fs_exists(const char *path) {
  FILINFO fno;
  bool root;
  return stat_of(path, &fno, &root);
}

bool fs_is_dir(const char *path) {
  FILINFO fno;
  bool root;
  return stat_of(path, &fno, &root) && (root || (fno.fattrib & AM_DIR));
}

bool fs_mkdir(const char *path) {
  char fp[PATH_MAX_FS];
  if (prepare(path, fp))
    return false;
  /* Each parent in turn, from the first after "0:/". */
  for (char *p = fp + 3; *p; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    f_mkdir(fp);
    *p = '/';
  }
  f_mkdir(fp);
  return fs_is_dir(fp);
}
