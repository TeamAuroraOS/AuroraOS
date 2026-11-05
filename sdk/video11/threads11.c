/* What FFmpeg's frame threads need, with one thread to a core and no scheduler: a
 * "thread" takes over a waiting core until its function returns, and waiting is
 * spinning, since there is nothing else for that core to do. The job's memory is
 * coherent between the cores, so LDREX/STREX work; WFE keeps a wait cheap on the
 * bus, and every release signals with SEV. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#include "engine.h"
#include "smp.h"

#define MAX_THREADS SMP_CORES

typedef struct __pthread_t {
  void *(*volatile fn)(void *);
  void *volatile arg;
  void *volatile result;
  volatile uint32_t state;
  uint32_t core;
} Thread;

enum { T_FREE, T_ASSIGNED, T_RUNNING, T_DONE };

static Thread threads[MAX_THREADS];
static volatile uint32_t pool_stop;

static inline void wfe(void) { __asm__ volatile("wfe"); }

static void spin_lock(volatile int32_t *l) {
  int32_t v, fail;
  for (;;) {
    __asm__ volatile("ldrex %0, [%2]\n\t"
                     "teq %0, #0\n\t"
                     "strexeq %1, %3, [%2]\n\t"
                     "movne %1, #1"
                     : "=&r"(v), "=&r"(fail)
                     : "r"(l), "r"(1)
                     : "cc", "memory");
    if (!fail) {
      cpu_dmb();
      return;
    }
    if (v)
      wfe();
  }
}

static int spin_try(volatile int32_t *l) {
  int32_t v, fail;
  __asm__ volatile("ldrex %0, [%2]\n\t"
                   "teq %0, #0\n\t"
                   "strexeq %1, %3, [%2]\n\t"
                   "movne %1, #1"
                   : "=&r"(v), "=&r"(fail)
                   : "r"(l), "r"(1)
                   : "cc", "memory");
  if (fail)
    return 0;
  cpu_dmb();
  return 1;
}

static void spin_unlock(volatile int32_t *l) {
  cpu_dmb();
  *l = 0;
  cpu_sev();
}

static inline uint32_t this_core(void) {
  uint32_t id;
  __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(id));
  return id & 3u;
}

/* newlib's locks, which malloc takes. */
void __libc_lock_acquire(_LOCK_T *lock) { spin_lock(lock); }
void __libc_lock_release(_LOCK_T *lock) { spin_unlock(lock); }
int __libc_lock_try_acquire(_LOCK_T *lock) { return spin_try(lock) ? 0 : 1; }

void __libc_lock_acquire_recursive(_LOCK_RECURSIVE_T *l) {
  uint32_t me = this_core() + 1u;
  if (l->lock && l->thread_tag == me) {
    l->counter++;
    return;
  }
  spin_lock(&l->lock);
  l->thread_tag = me;
  l->counter = 1;
}

void __libc_lock_release_recursive(_LOCK_RECURSIVE_T *l) {
  if (--l->counter)
    return;
  l->thread_tag = 0;
  spin_unlock(&l->lock);
}

int __libc_lock_try_acquire_recursive(_LOCK_RECURSIVE_T *l) {
  uint32_t me = this_core() + 1u;
  if (l->lock && l->thread_tag == me) {
    l->counter++;
    return 0;
  }
  if (!spin_try(&l->lock))
    return 1;
  l->thread_tag = me;
  l->counter = 1;
  return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
  (void)a;
  m->type = PTHREAD_MUTEX_NORMAL;
  m->normal = 0;
  return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) {
  (void)m;
  return 0;
}
int pthread_mutex_lock(pthread_mutex_t *m) {
  spin_lock(&m->normal);
  return 0;
}
int pthread_mutex_trylock(pthread_mutex_t *m) {
  return spin_try(&m->normal) ? 0 : EBUSY;
}
int pthread_mutex_unlock(pthread_mutex_t *m) {
  spin_unlock(&m->normal);
  return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
  (void)a;
  c->cond = 0;
  return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) {
  (void)c;
  return 0;
}

/* A counter bumped under no lock of its own: a waiter leaves when it has changed,
 * which at worst wakes one more than asked, and FFmpeg's callers all loop. */
static void cond_bump(pthread_cond_t *c) {
  uint32_t v, fail;
  __asm__ volatile("1:\n\t"
                   "ldrex %0, [%2]\n\t"
                   "add %0, %0, #1\n\t"
                   "strex %1, %0, [%2]\n\t"
                   "teq %1, #0\n\t"
                   "bne 1b"
                   : "=&r"(v), "=&r"(fail)
                   : "r"(&c->cond)
                   : "cc", "memory");
  cpu_sev();
}
int pthread_cond_signal(pthread_cond_t *c) {
  cond_bump(c);
  return 0;
}
int pthread_cond_broadcast(pthread_cond_t *c) {
  cond_bump(c);
  return 0;
}
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  uint32_t seen = *(volatile uint32_t *)&c->cond;
  pthread_mutex_unlock(m);
  while (*(volatile uint32_t *)&c->cond == seen)
    wfe();
  pthread_mutex_lock(m);
  return 0;
}
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *t) {
  (void)t;
  return pthread_cond_wait(c, m);
}

int pthread_once(pthread_once_t *o, void (*fn)(void)) {
  /* 0 not run, 1 running, 2 finished. */
  volatile int *st = (volatile int *)&o->status;
  int expect = 0;
  if (__atomic_compare_exchange_n(st, &expect, 1, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    fn();
    *st = 2;
    cpu_sev();
  } else {
    while (*st != 2)
      wfe();
  }
  return 0;
}

int pthread_create(pthread_t *out, const pthread_attr_t *a, void *(*fn)(void *),
                   void *arg) {
  (void)a;
  for (uint32_t i = 0; i < MAX_THREADS; i++) {
    Thread *t = &threads[i];
    if (t->core && t->state == T_FREE) {
      t->fn = fn;
      t->arg = arg;
      cpu_dmb();
      t->state = T_ASSIGNED;
      cpu_sev();
      *out = t;
      return 0;
    }
  }
  return EAGAIN;
}

int pthread_join(pthread_t t, void **res) {
  while (t->state != T_DONE)
    wfe();
  if (res)
    *res = t->result;
  cpu_dmb();
  t->state = T_FREE;
  cpu_sev();
  return 0;
}

pthread_t pthread_self(void) {
  uint32_t me = this_core();
  for (uint32_t i = 0; i < MAX_THREADS; i++)
    if (threads[i].core == me)
      return &threads[i];
  return &threads[0];
}

void threads11_setup(uint32_t cores_mask) {
  uint32_t n = 0;
  for (uint32_t c = 2u; c < SMP_CORES; c++)
    if (cores_mask & (1u << c))
      threads[n++].core = c;
}

/* A core other than the first: runs threads until the job ends. */
void threads11_core_loop(uint32_t core) {
  Thread *t = 0;
  for (uint32_t i = 0; i < MAX_THREADS; i++)
    if (threads[i].core == core)
      t = &threads[i];
  if (!t)
    return;
  while (!pool_stop) {
    if (t->state != T_ASSIGNED) {
      wfe();
      continue;
    }
    t->state = T_RUNNING;
    t->result = t->fn(t->arg);
    cpu_dmb();
    t->state = T_DONE;
    cpu_sev();
  }
}

void threads11_stop(void) {
  pool_stop = 1u;
  cpu_sev();
}

int smp_stop(void) { return ((volatile SmpBlock *)SMP_BLOCK_ADDR)->stop != 0u; }
