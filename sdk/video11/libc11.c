/* The job's start and the little newlib needs under it. There is no OS here: the
 * heap is a stretch of FCRAM, nothing prints and nothing opens files. Run by every
 * core of the job, which the core starts with its map and caches on. */
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

#include "engine.h"
#include "smp.h"
#include "video.h"

extern char __bss_start__[], __bss_end__[];
extern void __libc_init_array(void);

static uint32_t heap_brk, heap_limit;

void *_sbrk_r(struct _reent *r, ptrdiff_t incr) {
  uint32_t prev = heap_brk;
  if (incr < 0 ? (uint32_t)-incr > heap_brk - VIDEO_HEAP_ADDR
               : (uint32_t)incr > heap_limit - heap_brk) {
    r->_errno = ENOMEM;
    return (void *)-1;
  }
  heap_brk += (uint32_t)incr;
  return (void *)prev;
}

struct _reent *__getreent(void) { return _impure_ptr; }

_ssize_t _write_r(struct _reent *r, int fd, const void *p, size_t n) {
  (void)r;
  (void)fd;
  (void)p;
  return (_ssize_t)n;
}
_ssize_t _read_r(struct _reent *r, int fd, void *p, size_t n) {
  (void)fd;
  (void)p;
  (void)n;
  r->_errno = ENOSYS;
  return -1;
}
int _open_r(struct _reent *r, const char *p, int f, int m) {
  (void)p;
  (void)f;
  (void)m;
  r->_errno = ENOSYS;
  return -1;
}
int _close_r(struct _reent *r, int fd) {
  (void)r;
  (void)fd;
  return 0;
}
off_t _lseek_r(struct _reent *r, int fd, off_t o, int w) {
  (void)fd;
  (void)o;
  (void)w;
  r->_errno = ENOSYS;
  return -1;
}
int _fstat_r(struct _reent *r, int fd, struct stat *st) {
  (void)r;
  (void)fd;
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFCHR;
  return 0;
}
int _isatty_r(struct _reent *r, int fd) {
  (void)r;
  (void)fd;
  return 1;
}
int _getpid_r(struct _reent *r) {
  (void)r;
  return 1;
}
int _kill_r(struct _reent *r, int pid, int sig) {
  (void)pid;
  (void)sig;
  r->_errno = ENOSYS;
  return -1;
}
int _gettimeofday_r(struct _reent *r, struct timeval *t, void *z) {
  (void)r;
  (void)z;
  t->tv_sec = 0;
  t->tv_usec = 0;
  return 0;
}
clock_t _times_r(struct _reent *r, struct tms *t) {
  (void)r;
  memset(t, 0, sizeof(*t));
  return 0;
}
int _unlink_r(struct _reent *r, const char *p) {
  (void)p;
  r->_errno = ENOSYS;
  return -1;
}
int _link_r(struct _reent *r, const char *a, const char *b) {
  (void)a;
  (void)b;
  r->_errno = ENOSYS;
  return -1;
}
int _stat_r(struct _reent *r, const char *p, struct stat *st) {
  (void)p;
  (void)st;
  r->_errno = ENOSYS;
  return -1;
}
int _rename_r(struct _reent *r, const char *a, const char *b) {
  (void)a;
  (void)b;
  r->_errno = ENOSYS;
  return -1;
}
long sysconf(int name) {
  (void)name;
  return 1;
}

/* A fault or a failed assert has nowhere to go: this core stops, and with it the
 * heartbeat that VideoStats shows. */
void _exit(int code) {
  (void)code;
  for (;;)
    __asm__ volatile("wfe");
}
void abort(void) { _exit(1); }

static volatile uint32_t ready;

/* Called on each core by the stub in the core: entry(arg, core). The first core
 * clears the image's .bss and sets up the library before the others may touch it,
 * and those wait on a word kept out of .bss for that reason. */
static volatile uint32_t go_word __attribute__((section(".data"))) = 0;

__attribute__((section(".text.entry"))) void video11_entry(uint32_t cores_mask, uint32_t core) {
  VideoShared *s = (VideoShared *)VIDEO_SHARED_ADDR;
  (void)ready;
  if (core != 1u) {
    while (!go_word)
      __asm__ volatile("wfe");
    threads11_core_loop(core);
    return;
  }
  for (char *p = __bss_start__; p < __bss_end__; p++)
    *p = 0;
  heap_brk = VIDEO_HEAP_ADDR;
  heap_limit = cores_mask >> 16 ? (cores_mask & 0xFFFF0000u) : VIDEO_HEAP_END_OLD;
  threads11_setup(cores_mask & 0xFFFFu);
  __libc_init_array();
  cpu_dsb();
  go_word = 1u;
  cpu_sev();
  s->cores_used = cores_mask & 0xFFFFu;
  s->state = VIDEO_ST_STARTING;
  video11_main();
}

/* __libc_init_array calls these; the toolchain's crt files are not linked. */
void _init(void) {}
void _fini(void) {}
