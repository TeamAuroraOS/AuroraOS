/* The other ARM11 cores: bringing them up and handing them jobs. Core 0 runs
 * this; the cores themselves run the loop smp11_stub in audio11_start.s, copied
 * out of this image, and then the app's code.
 *
 * The start-up follows libn3ds (source/arm11/drivers/pdn.c, by derrek and
 * profi200, GPL-3.0) and its comment that it must happen with no map on any
 * core. Core 1 waits in the boot ROM for an entry at 0x1FFFFFDC and a software
 * interrupt, the path open_agb_firm uses on every model. Cores 2 and 3 are held
 * in reset until the boot ROM overlay points them at an entry; libn3ds keeps
 * that path compiled out, so here every step of it is bounded and a core that
 * does not answer is only reported. */
#include "core11.h"
#include "smp.h"

#define SCU_CTRL     0x17E00000u
#define SCU_CONFIG   0x17E00004u
#define SCU_CPU_STAT 0x17E00008u
#define SCU_INVAL    0x17E0000Cu
#define SCU_EN       (1u << 0)
/* Every alias and SCU register open to every core: libn3ds's SCU_CTRL_RST_VAL. */
#define SCU_ACCESS   0x1FFEu
#define SCU_ALL_WAYS 0xFFFFu
#define SCU_CPUS(v)  (((v) & 3u) + 1u)

#define GICD_CTRL    0x17E01000u
#define GICD_SOFTINT 0x17E01F00u

#define BOOT11_CORE1_ENTRY 0x1FFFFFDCu

#define CFG11_OVERLAY_CNT 0x10140420u
#define CFG11_OVERLAY_VAL 0x10140424u
#define PDN_LGR_CPU_CNT   0x10141310u /* u8, one per core */
#define CPU_NORST         (1u << 0)
#define CPU_D_OVERLAY     (1u << 1)
#define CPU_RST_STAT      (1u << 4)

#define CFG11_MPCORE_CLKCNT 0x10141300u
#define CLKCNT_CUR(v) (((v) >> 16) & 7u)
#define MODE_268_LGR2 1u /* cores 2 and 3 come up at this clock */
#define SOCINFO_LGR2  (1u << 2)

/* A first-level section with the ARMv6 descriptor format, domain 0. */
#define TT_SECTION  (2u << 0)
#define TT_B        (1u << 2)
#define TT_C        (1u << 3)
#define TT_AP_RW    (3u << 10)
#define TT_TEX(x)   ((x) << 12)
#define TT_S        (1u << 16)
#define TT_CACHED   (TT_TEX(1u) | TT_C | TT_B | TT_S)
#define TT_UNCACHED (TT_TEX(1u) | TT_S)
#define TT_IMAGE    (TT_TEX(1u) | TT_C | TT_B)

#define CORE_FIRST_MB (AUDIO_CORE_ADDR >> 20)
#define CORE_MBS      2u

#define ANSWER_US   50000u  /* a core already waiting, or core 1 leaving boot11 */
#define RESET_US    100000u /* cores 2 and 3 leaving reset */
#define JOBS_END_US 1000000u

/* Where the self-test works: free while the Terminal runs (aShop's music has it
 * only while the shop is open). */
#define TEST_ADDR 0x27F00000u
#define TEST_MB   (TEST_ADDR >> 20)
#define TEST_SP(c) (TEST_ADDR + 0x4000u + 0x1000u * (c))
#define TEST_MEM   (TEST_ADDR + 0x10000u)

extern const uint8_t smp11_stub[];
extern const uint8_t smp11_stub_end[];

static SmpBlock *const b = (SmpBlock *)SMP_BLOCK_ADDR;
static uint32_t *const tt = (uint32_t *)SMP_TT_ADDR;

static void stub_install(void) {
  const uint8_t *s = smp11_stub;
  volatile uint8_t *d = (volatile uint8_t *)SMP_STUB_ADDR;
  uint32_t n = (uint32_t)(smp11_stub_end - smp11_stub);
  for (uint32_t i = 0; i < n; i++)
    if (d[i] != s[i])
      d[i] = s[i];
  dsb();
}

static void wake(uint32_t core) {
  MMIO32(GICD_CTRL) |= 1u;
  MMIO32(GICD_SOFTINT) = (1u << (16u + core)) | core;
  dsb();
}

static int wait_alive(uint32_t core, uint32_t us) {
  uint32_t t0 = core11_now_us();
  while (core11_now_us() - t0 < us)
    if (b->alive[core] == SMP_ALIVE)
      return 1;
  return 0;
}

/* FCRAM keeps `alive` across a reset, so it only counts as a fresh answer. A core
 * waiting in the loop goes round it once; core 1 still in boot11 takes the entry
 * and arrives in the loop. Either way it ends up saying so. */
static int ping(uint32_t core) {
  b->alive[core] = 0;
  dsb();
  wake(core);
  return wait_alive(core, ANSWER_US);
}

static int core1_up(void) {
  MMIO32(BOOT11_CORE1_ENTRY) = SMP_STUB_ADDR;
  return ping(1u);
}

static inline uint8_t cpu_cnt(uint32_t core) {
  return MMIO8(PDN_LGR_CPU_CNT + core);
}

static void cores23_up(AudioCtrl *ct, uint32_t want) {
  uint32_t todo = 0, t0, before, after, status, mode;

  for (uint32_t c = 2u; c < SMP_CORES; c++) {
    if (!(want & (1u << c)))
      continue;
    if (cpu_cnt(c) & CPU_RST_STAT) {
      if (ping(c))
        b->started |= 1u << c;
      else
        b->failed |= 1u << c;
    } else {
      todo |= 1u << c;
    }
  }
  if (!todo)
    return;

  before = ct->n3ds_before;
  after = ct->n3ds_after;
  status = ct->n3ds_status;
  mode = CLKCNT_CUR(MMIO32(CFG11_MPCORE_CLKCNT));
  /* Normal mode in the SCU: two bits per core. */
  MMIO32(SCU_CPU_STAT) &= ~((3u << 4) | (3u << 6));
  dsb();
  if (mode != MODE_268_LGR2) {
    clock11_set(ct, MODE_268_LGR2);
    core11_clock_restart(ct);
  }

  MMIO8(CFG11_OVERLAY_CNT) = 1u;
  MMIO32(CFG11_OVERLAY_VAL) = SMP_STUB_ADDR;
  dsb();
  for (uint32_t c = 2u; c < SMP_CORES; c++)
    if (todo & (1u << c)) {
      b->alive[c] = 0;
      MMIO8(PDN_LGR_CPU_CNT + c) = CPU_D_OVERLAY | CPU_NORST;
    }
  dsb();
  /* The loop clears the data overlay bit as its first word to core 0. */
  t0 = core11_now_us();
  for (;;) {
    int waiting = 0;
    for (uint32_t c = 2u; c < SMP_CORES; c++)
      if ((todo & (1u << c)) &&
          (cpu_cnt(c) & (CPU_RST_STAT | CPU_D_OVERLAY)) != CPU_RST_STAT)
        waiting = 1;
    if (!waiting || core11_now_us() - t0 >= RESET_US)
      break;
  }
  MMIO8(CFG11_OVERLAY_CNT) = 0;
  dsb();

  if (mode != MODE_268_LGR2) {
    clock11_set(ct, mode);
    core11_clock_restart(ct);
  }
  ct->n3ds_before = before;
  ct->n3ds_after = after;
  ct->n3ds_status = status;

  for (uint32_t c = 2u; c < SMP_CORES; c++)
    if (todo & (1u << c)) {
      if (wait_alive(c, ANSWER_US))
        b->started |= 1u << c;
      else
        b->failed |= 1u << c;
    }
}

/* With this core's map off; the caller puts it back. */
static void cores_up(AudioCtrl *ct, uint32_t want) {
  uint32_t cpus;

  stub_install();
  if (!(MMIO32(SCU_CTRL) & SCU_EN)) {
    /* Tag RAMs first, as the MPCore manual asks before any core joins. */
    MMIO32(SCU_CTRL) = SCU_ACCESS;
    MMIO32(SCU_INVAL) = SCU_ALL_WAYS;
    MMIO32(SCU_CTRL) = SCU_ACCESS | SCU_EN;
    dsb();
  }
  cpus = SCU_CPUS(MMIO32(SCU_CONFIG));
  b->started = 0;
  b->failed = 0;
  if (want & 2u) {
    if (cpus >= 2u && core1_up())
      b->started |= 2u;
    else
      b->failed |= 2u;
  }
  if (cpus >= 4u && (ct->socinfo & SOCINFO_LGR2))
    cores23_up(ct, want);
  else
    b->failed |= want & 0xCu;
}

static void tt_build(const SmpMapEntry *map, uint32_t count) {
  for (uint32_t mb = 0; mb < 4096u; mb++)
    tt[mb] = (mb << 20) | TT_SECTION | TT_AP_RW;
  /* Only what the self-test runs from: a job brings its own code. */
  for (uint32_t i = 0; i < CORE_MBS; i++)
    tt[CORE_FIRST_MB + i] |= TT_IMAGE;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t attr = map[i].attr == SMP_MAP_CACHED ? TT_CACHED : TT_UNCACHED;
    for (uint32_t k = 0; k < map[i].mbs; k++) {
      uint32_t mb = map[i].first_mb + k;
      if (mb < 4096u)
        tt[mb] = (mb << 20) | TT_SECTION | TT_AP_RW | attr;
    }
  }
  dsb();
}

/* Running jobs must have ended; nothing of the last one may be in the L2. */
static void job_start(const SmpJob *j) {
  SmpJob copy;
  const uint32_t *s = (const uint32_t *)j;
  uint32_t *d = (uint32_t *)&copy;
  for (uint32_t i = 0; i < sizeof(copy) / 4u; i++)
    d[i] = s[i];
  if (copy.map_count > SMP_MAP_MAX)
    copy.map_count = SMP_MAP_MAX;

  tt_build(copy.map, copy.map_count);
  b->stop = 0;
  dsb();
  mmu11_off();
  mmu11_on();
  for (uint32_t c = 1u; c < SMP_CORES; c++) {
    if (!(copy.cores & b->started & (1u << c)))
      continue;
    b->arg[c] = copy.arg;
    b->sp[c] = copy.sp[c];
    dsb();
    b->entry[c] = copy.entry;
    dsb();
    wake(c);
  }
}

static int jobs_running(void) {
  for (uint32_t c = 1u; c < SMP_CORES; c++)
    if (b->running[c] || b->entry[c])
      return 1;
  return 0;
}

static void jobs_end(void) {
  uint32_t t0 = core11_now_us();
  b->stop = 1u;
  dsb();
  while (jobs_running() && core11_now_us() - t0 < JOBS_END_US)
    ;
  video11_reset();
  mmu11_off();
  mmu11_on();
}

/* Runs on the other cores, from this image's text: no data of this image. */
static inline uint32_t ptimer11(void) {
  return ~MMIO32(0x17E00604u);
}

static void test_job(uint32_t addr, uint32_t core) {
  SmpBlock *block = (SmpBlock *)SMP_BLOCK_ADDR;
  volatile uint32_t *sum = (volatile uint32_t *)addr;
  volatile uint32_t *mem =
      (volatile uint32_t *)(TEST_MEM + (core - 1u) * SMP_TEST_BYTES);
  uint32_t t0, tmp, fail;

  /* This core's own timer, at half its clock, no prescaler. */
  MMIO32(0x17E00608u) = 0;
  MMIO32(0x17E00600u) = 0xFFFFFFFFu;
  MMIO32(0x17E00608u) = 2u | 1u;

  t0 = ptimer11();
  for (uint32_t i = 0; i < SMP_TEST_LOOPS; i++)
    __asm__ volatile("" ::"r"(i));
  block->test_cpu[core] = ptimer11() - t0;

  /* Every core adding to one word shows the cores really share their caches. */
  for (uint32_t i = 0; i < 100000u; i++) {
    __asm__ volatile("1:\n\t"
                     "ldrex %0, [%2]\n\t"
                     "add %0, %0, #1\n\t"
                     "strex %1, %0, [%2]\n\t"
                     "cmp %1, #0\n\t"
                     "bne 1b"
                     : "=&r"(tmp), "=&r"(fail)
                     : "r"(sum)
                     : "cc", "memory");
  }

  t0 = ptimer11();
  for (uint32_t p = 0; p < SMP_TEST_PASSES; p++)
    for (uint32_t i = 0; i < SMP_TEST_BYTES / 4u; i++)
      mem[i] = mem[i] + core;
  block->test_mem[core] = ptimer11() - t0;

  block->test_done[core] = 1u;
  MMIO32(0x17E00608u) = 0;
}

static void test_run(AudioCtrl *ct) {
  SmpMapEntry map = {TEST_MB, 1u, SMP_MAP_CACHED};
  volatile uint32_t *sum = (volatile uint32_t *)TEST_ADDR;
  uint32_t cores, cpus, count = 0, t0;

  mmu11_off();
  cpus = SCU_CPUS(MMIO32(SCU_CONFIG));
  cores_up(ct, cpus >= 4u ? 0xEu : cpus >= 2u ? 2u : 0u);
  mmu11_on();

  *sum = 0;
  dsb();
  tt_build(&map, 1u);
  b->stop = 0;
  for (uint32_t c = 0; c < SMP_CORES; c++)
    b->test_cpu[c] = b->test_mem[c] = b->test_done[c] = 0;
  mmu11_off();
  mmu11_on();
  cores = b->started;
  for (uint32_t c = 1u; c < SMP_CORES; c++) {
    if (!(cores & (1u << c)))
      continue;
    count++;
    b->arg[c] = TEST_ADDR;
    b->sp[c] = TEST_SP(c);
    dsb();
    b->entry[c] = (uint32_t)test_job;
    dsb();
    wake(c);
  }
  t0 = core11_now_us();
  while (jobs_running() && core11_now_us() - t0 < 4000000u)
    ;
  /* Their writes are in the L2 or the caches they just cleaned. */
  mmu11_off();
  mmu11_on();
  b->test_sum = *sum;
  b->test_want = count * 100000u;
  b->test_runs = b->test_runs + 1u;
}

void smp11_command(AudioCtrl *ct, uint32_t op, uint32_t arg) {
  if (op == SMP_OP_CORES) {
    mmu11_off();
    cores_up(ct, arg & 0xEu);
    mmu11_on();
  } else if (op == SMP_OP_RUN) {
    job_start((const SmpJob *)arg);
  } else if (op == SMP_OP_END) {
    jobs_end();
  } else if (op == SMP_OP_TEST) {
    test_run(ct);
  }
}
