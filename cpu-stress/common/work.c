/* The workloads (work.h). Build with -fno-tree-loop-distribute-patterns, so
 * GCC keeps the memory loops as written rather than calling a C library's
 * memset or memcpy, which would differ between the systems. */
#include "work.h"

/* Never inlined: GCC sizes the .rept as one instruction, and a jump table
 * around it can come out too short. */
static __attribute__((noinline)) uint32_t w_clock(uint32_t passes) {
  uint32_t x = 0;
#if defined(__arm__)
  __asm__ volatile("1:\n\t"
                   ".rept 256\n\t"
                   "add %0, %0, #1\n\t"
                   ".endr\n\t"
                   "subs %1, %1, #1\n\t"
                   "bne 1b"
                   : "+r"(x), "+r"(passes)
                   :
                   : "cc");
#else
  for (volatile uint32_t i = 0; i < passes * 256u; i++)
    x++;
#endif
  return x;
}

/* Shifts, multiplies, rotates and a branch the data decides. */
static uint32_t w_int(uint32_t n, uint32_t seed) {
  uint32_t a = seed | 1u, b = 0x9E3779B9u, c = 0;
  while (n--) {
    a ^= a << 13;
    a ^= a >> 17;
    a ^= a << 5;
    b += a * 0x85EBCA6Bu;
    c ^= (b >> 7) + (a & 0xFFu);
    c = (c << 3) | (c >> 29);
    if (c & 1u)
      b ^= c;
  }
  return a ^ b ^ c;
}

/* Four multiplies and four adds an iteration, in two dependent chains; the
 * sums settle, so nothing overflows or goes denormal. */
static float w_float(uint32_t n) {
  float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
  while (n--) {
    s0 = s0 * 0.999f + 1.0f;
    s1 = s1 * 0.998f + s0;
    s2 = s2 * 0.997f + 1.0f;
    s3 = s3 * 0.996f + s2;
  }
  return s0 + s1 + s2 + s3;
}

static uint32_t w_read(const uint32_t *p, uint32_t bytes) {
  uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  for (const uint32_t *e = p + bytes / 4u; p < e; p += 8) {
    s0 += p[0] + p[4];
    s1 += p[1] + p[5];
    s2 += p[2] + p[6];
    s3 += p[3] + p[7];
  }
  return s0 ^ s1 ^ s2 ^ s3;
}

static void w_write(uint32_t *p, uint32_t bytes, uint32_t v) {
  for (uint32_t *e = p + bytes / 4u; p < e; p += 8) {
    p[0] = v;
    p[1] = v;
    p[2] = v;
    p[3] = v;
    p[4] = v;
    p[5] = v;
    p[6] = v;
    p[7] = v;
  }
}

static void w_copy(uint32_t *d, const uint32_t *s, uint32_t bytes) {
  for (const uint32_t *e = s + bytes / 4u; s < e; s += 8, d += 8) {
    uint32_t a = s[0], b = s[1], c = s[2], e2 = s[3];
    uint32_t f = s[4], h = s[5], i = s[6], j = s[7];
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e2;
    d[4] = f;
    d[5] = h;
    d[6] = i;
    d[7] = j;
  }
}

uint32_t work_run(WorkCtx *c, uint32_t kind, uint32_t reps) {
  uint32_t sum = 0;
  while (reps--) {
    switch (kind) {
      case W_CLOCK:
        sum += w_clock(CLOCK_PASSES);
        break;
      case W_INT:
        c->seed = w_int(INT_CHUNK, c->seed);
        sum += c->seed;
        break;
      case W_FLOAT:
        sum += (uint32_t)w_float(FLOAT_CHUNK);
        break;
      case W_READ:
        sum += w_read(c->big_a, MEM_BYTES);
        break;
      case W_WRITE:
        w_write(c->big_b, MEM_BYTES, c->seed + reps);
        break;
      case W_COPY:
        w_copy(c->big_b, c->big_a, MEM_BYTES);
        break;
      case W_ROUND:
        c->seed = w_int(ROUND_INT, c->seed);
        sum += (uint32_t)w_float(ROUND_FLOAT);
        w_copy(c->rb, c->ra, ROUND_MEM);
        sum += c->rb[c->seed & (ROUND_MEM / 4u - 1u)];
        break;
    }
  }
  c->sink += sum;
  return sum;
}
