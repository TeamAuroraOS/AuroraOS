/* The system calls under devkitARM's newlib: the console for stdout and
 * stderr, FatFs for files, the RTC and the ARM9 timer for time, and the heap.
 * Apps run alone on one CPU, so the locks do nothing. */
#include "app_internal.h"
#include "timer.h"

#include <errno.h>
#include <reent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

/* Free while an app runs: the OS stages apps here before starting them, and
 * 0x25000000 on is the launch and HOME return stubs. */
#define HEAP_START 0x24000000u
#define HEAP_END   0x25000000u

struct _reent *__getreent(void) { return _impure_ptr; }

void __libc_lock_acquire(_LOCK_T *lock) { (void)lock; }
void __libc_lock_release(_LOCK_T *lock) { (void)lock; }
void __libc_lock_acquire_recursive(_LOCK_RECURSIVE_T *lock) { (void)lock; }
void __libc_lock_release_recursive(_LOCK_RECURSIVE_T *lock) { (void)lock; }
int __libc_lock_try_acquire(_LOCK_T *lock) {
  (void)lock;
  return 0;
}
int __libc_lock_try_acquire_recursive(_LOCK_RECURSIVE_T *lock) {
  (void)lock;
  return 0;
}

void *_sbrk_r(struct _reent *r, ptrdiff_t incr) {
  static u32 brk = HEAP_START;
  u32 prev = brk;
  if (incr < 0 ? (u32)-incr > brk - HEAP_START : (u32)incr > HEAP_END - brk) {
    r->_errno = ENOMEM;
    return (void *)-1;
  }
  brk += (u32)incr;
  return (void *)prev;
}

/* A negative errno from app_fs_* into newlib's -1 and errno. */
static int fail(struct _reent *r, int e) {
  if (e < 0) {
    r->_errno = -e;
    return -1;
  }
  return e;
}

int _open_r(struct _reent *r, const char *path, int flags, int mode) {
  (void)mode;
  return fail(r, app_fs_open(path, flags));
}

int _close_r(struct _reent *r, int fd) {
  return fd < APP_FD_FIRST ? 0 : fail(r, app_fs_close(fd));
}

_ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t n) {
  if (fd == STDIN_FILENO)
    return 0;
  if (fd < APP_FD_FIRST)
    return fail(r, -EBADF);
  return fail(r, app_fs_read(fd, buf, n));
}

_ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t n) {
  if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
    app_con_write(buf, n);
    return (_ssize_t)n;
  }
  if (fd < APP_FD_FIRST)
    return fail(r, -EBADF);
  return fail(r, app_fs_write(fd, buf, n));
}

_off_t _lseek_r(struct _reent *r, int fd, _off_t off, int whence) {
  long pos;
  if (fd < APP_FD_FIRST)
    return fail(r, -ESPIPE);
  pos = app_fs_lseek(fd, off, whence);
  if (pos < 0) {
    r->_errno = (int)-pos;
    return -1;
  }
  return pos;
}

int _fstat_r(struct _reent *r, int fd, struct stat *st) {
  if (fd < APP_FD_FIRST) {
    *st = (struct stat){0};
    st->st_mode = S_IFCHR;
    return 0;
  }
  return fail(r, app_fs_fstat(fd, st));
}

int _stat_r(struct _reent *r, const char *path, struct stat *st) {
  return fail(r, app_fs_stat(path, st));
}

/* The console counts as a terminal, which makes stdout line-buffered. */
int _isatty_r(struct _reent *r, int fd) {
  if (fd < APP_FD_FIRST)
    return 1;
  r->_errno = ENOTTY;
  return 0;
}

int _unlink_r(struct _reent *r, const char *path) {
  return fail(r, app_fs_unlink(path));
}

int _rmdir_r(struct _reent *r, const char *path) {
  return fail(r, app_fs_rmdir(path));
}

int _rename_r(struct _reent *r, const char *from, const char *to) {
  return fail(r, app_fs_rename(from, to));
}

int _mkdir_r(struct _reent *r, const char *path, int mode) {
  (void)mode;
  return fail(r, app_fs_mkdir(path));
}

int mkdir(const char *path, mode_t mode) {
  return _mkdir_r(_REENT, path, (int)mode);
}

/* What devkitARM keeps in libsysbase rather than newlib. */
int rmdir(const char *path) { return _rmdir_r(_REENT, path); }

int lstat(const char *path, struct stat *st) {
  return _stat_r(_REENT, path, st);
}

int ftruncate(int fd, off_t len) {
  return fail(_REENT, app_fs_ftruncate(fd, (long)len));
}

int fsync(int fd) {
  return fd < APP_FD_FIRST ? 0 : fail(_REENT, app_fs_fsync(fd));
}

int chdir(const char *path) { return fail(_REENT, app_fs_chdir(path)); }

char *getcwd(char *buf, size_t size) {
  const char *cwd = app_fs_getcwd();
  size_t n = strlen(cwd) + 1;
  if (!buf) {
    if (size < n)
      size = n;
    buf = malloc(size);
    if (!buf) {
      errno = ENOMEM;
      return NULL;
    }
  }
  if (size < n) {
    errno = size ? ERANGE : EINVAL;
    return NULL;
  }
  memcpy(buf, cwd, n);
  return buf;
}

/* The RTC has whole seconds: it is read once, and the timer counts on from
 * there, so the time moves smoothly. */
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) {
  static long long base_secs = -1;
  static u64 base_ticks;
  u64 us;
  (void)tz;
  if (base_secs < 0) {
    SysTime t;
    if (!sys_time(&t)) {
      r->_errno = EIO;
      return -1;
    }
    base_secs = app_civil_secs(t.year, t.month, t.day, t.hour, t.minute,
                               t.second);
    base_ticks = app_ticks64();
  }
  if (tv) {
    us = (app_ticks64() - base_ticks) * 1000000u / timer_hz();
    tv->tv_sec = (time_t)(base_secs + (long long)(us / 1000000u));
    tv->tv_usec = (suseconds_t)(us % 1000000u);
  }
  return 0;
}

clock_t _times_r(struct _reent *r, struct tms *t) {
  clock_t c = (clock_t)(app_ticks64() * CLOCKS_PER_SEC / timer_hz());
  (void)r;
  if (t) {
    t->tms_utime = c;
    t->tms_stime = 0;
    t->tms_cutime = 0;
    t->tms_cstime = 0;
  }
  return c;
}

unsigned sleep(unsigned secs) {
  app_wait_ticks((u64)secs * timer_hz());
  return 0;
}

int usleep(useconds_t us) {
  app_wait_ticks((u64)us * timer_hz() / 1000000u);
  return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
  if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
    errno = EINVAL;
    return -1;
  }
  app_wait_ticks((u64)req->tv_sec * timer_hz() +
                 (u64)req->tv_nsec * timer_hz() / 1000000000u);
  if (rem)
    rem->tv_sec = rem->tv_nsec = 0;
  return 0;
}

int _getpid_r(struct _reent *r) {
  (void)r;
  return 1;
}

/* abort() and assert() come here (SIGABRT). What they printed is on the
 * console; it stays up until HOME, which leaves without running atexit()
 * functions on a program in an unknown state. */
static void stopped(void) {
  static const char msg[] = "\n\x1b[91mThe app stopped. Press HOME.\x1b[0m\n";
  g_app_leaving = true; /* HOME is handled here, not through exit() */
  app_flush_stdout();
  app_con_write(msg, sizeof(msg) - 1);
  app_present();
  for (;;) {
    if (app_from_home() && app_home_pressed())
      _exit(134);
    app_wait_ticks(timer_hz() / 30u);
  }
}

static void con_str(const char *s) { app_con_write(s, strlen(s)); }

/* newlib's own prints through fiprintf(), which would bring a second printf
 * into every app: its dtoa() asserts. */
void __assert_func(const char *file, int line, const char *func,
                   const char *expr) {
  char num[12], *p = num + sizeof(num) - 1;
  unsigned n = line < 0 ? 0u : (unsigned)line;

  *p = '\0';
  do
    *--p = (char)('0' + n % 10u);
  while ((n /= 10u) && p > num);
  app_flush_stdout(); /* what the app printed before comes first */
  con_str("\nassertion \"");
  con_str(expr ? expr : "?");
  con_str("\" failed: file \"");
  con_str(file ? file : "?");
  con_str("\", line ");
  con_str(p);
  if (func) {
    con_str(", function: ");
    con_str(func);
  }
  con_str("\n");
  abort();
}

int _kill_r(struct _reent *r, int pid, int sig) {
  if (pid == 1 && sig == SIGABRT)
    stopped();
  if (pid == 1 && (sig == SIGTERM || sig == SIGINT || sig == SIGKILL))
    _exit(128 + sig);
  r->_errno = EINVAL;
  return -1;
}

void _exit(int code) { app_leave(code); }
