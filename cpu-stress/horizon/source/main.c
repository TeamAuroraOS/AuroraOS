// CPU stress test for Nintendo's 3DS firmware (Horizon), as homebrew on
// libctru: the shared test (../../common) on the ARM11, the CPU Horizon runs
// apps on. The workloads run on core 0, the app's thread; the stress also runs
// on every other core the app is given. On a New 3DS it switches to 804 MHz
// with the L2 cache off, as the AuroraOS version runs; X on the start screen
// turns the L2 cache on, or goes to 268 MHz. Results:
// SD:/Aurora/nintendo_stress.txt.
#include <3ds.h>

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "stress.h"

#define MAX_THREADS 3

/* Flush-to-zero and default NaN, as the AuroraOS job runs (job.c). */
#define FPSCR_RUNFAST 0x03000000u

enum { MODE_804, MODE_804_L2, MODE_268, MODES };

static bool g_n3ds, g_mcu, g_ptm, g_sysm;
static int g_mode = MODE_804;
static Thread g_threads[MAX_THREADS];
static WorkCtx g_ctx;
static uint64_t g_work_ticks;

static void runfast(void) { __asm__ volatile("vmsr fpscr, %0" ::"r"(FPSCR_RUNFAST)); }

uint64_t plat_ticks(void) { return svcGetSystemTick(); }
uint64_t plat_tick_hz(void) { return SYSCLOCK_ARM11; }

WorkCtx *plat_work_ctx(void) { return &g_ctx; }

void plat_work_start(uint32_t kind, uint32_t reps) {
  uint64_t t0 = svcGetSystemTick();
  work_run(&g_ctx, kind, reps);
  g_work_ticks = svcGetSystemTick() - t0;
  if (!g_work_ticks)
    g_work_ticks = 1;
}

uint64_t plat_work_wait(void) { return g_work_ticks; }

int plat_update(uint32_t *down) {
  static const struct {
    u32 hid, test;
  } map[] = {
      {KEY_A, K_A},
      {KEY_B, K_B},
      {KEY_X, K_X},
      {KEY_START, K_START},
      {KEY_L, K_L},
      {KEY_R, K_R},
      {KEY_DLEFT | KEY_CPAD_LEFT, K_LEFT},
      {KEY_DRIGHT | KEY_CPAD_RIGHT, K_RIGHT},
  };
  u32 k;
  if (!aptMainLoop())
    return 0;
  hidScanInput();
  k = hidKeysDown();
  *down = 0;
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (k & map[i].hid)
      *down |= map[i].test;
  return 1;
}

uint8_t *plat_fb(int bottom) {
  return gfxGetFramebuffer(bottom ? GFX_BOTTOM : GFX_TOP, GFX_LEFT, NULL,
                           NULL);
}

void plat_present(void) {
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
}

void plat_wait(uint32_t ms) { svcSleepThread((s64)ms * 1000000); }

/* The MCU's registers, as AuroraOS reads them (docs/power.md). */
void plat_battery(Battery *b) {
  u8 t, v[2], mv, f, c;
  b->temp_c = TEMP_UNKNOWN;
  b->tenths = -1;
  b->mv = -1;
  b->charging = -1;
  if (g_mcu) {
    if (R_SUCCEEDED(MCUHWC_ReadRegister(0x0A, &t, 1)))
      b->temp_c = (s8)t;
    if (R_SUCCEEDED(MCUHWC_ReadRegister(0x0B, v, 2)))
      b->tenths = v[0] >= 100 ? 1000 : v[0] * 10 + v[1] * 10 / 256;
    if (R_SUCCEEDED(MCUHWC_ReadRegister(0x0D, &mv, 1)))
      b->mv = mv * 5000 / 256;
    if (R_SUCCEEDED(MCUHWC_ReadRegister(0x0F, &f, 1)))
      b->charging = (f >> 4) & 1;
  }
  if (b->charging < 0 && g_ptm && R_SUCCEEDED(PTMU_GetBatteryChargeState(&c)))
    b->charging = c ? 1 : 0;
}

void plat_date(char *out, int size) {
  time_t now = time(NULL);
  struct tm *tm = gmtime(&now); /* Horizon's clock is local time already */
  if (!tm || !strftime(out, (size_t)size, "%Y-%m-%d %H:%M", tm))
    snprintf(out, (size_t)size, "unknown");
}

static const char *const mode_text[MODES] = {
    "804 MHz, L2 cache off", "804 MHz, L2 cache on", "268 MHz, L2 cache off"};

/* PTMSYSM_ConfigureNew3DSCPU: bit 0 the 804 MHz clock, bit 1 the L2 cache. */
static void mode_apply(void) {
  static const u8 value[MODES] = {1, 3, 0};
  if (!g_n3ds)
    return;
  if (g_sysm)
    PTMSYSM_ConfigureNew3DSCPU(value[g_mode]);
  else
    osSetSpeedupEnable(g_mode != MODE_268);
}

void plat_notes(char *out, int size) {
  if (!g_n3ds)
    snprintf(out, (size_t)size, "Old 3DS: 268 MHz");
  else if (g_sysm)
    snprintf(out, (size_t)size, "%s", mode_text[g_mode]);
  else
    snprintf(out, (size_t)size, "%s (ptm:sysm refused: libctru's speedup)",
             g_mode == MODE_268 ? "268 MHz" : "804 MHz, L2 cache on");
}

const char *plat_option(void) {
  static char line[48];
  if (!g_n3ds || !g_sysm)
    return 0;
  snprintf(line, sizeof(line), "X: %s", mode_text[(g_mode + 1) % MODES]);
  return line;
}

void plat_option_toggle(void) {
  g_mode = (g_mode + 1) % MODES;
  mode_apply();
}

static void worker_entry(void *arg) {
  runfast();
  stress_worker((StressWorker *)arg);
}

/* Core 1 is the system core, which an app gets a share of once it asks;
 * core 2 is the New 3DS's second app core; core 3 is the system's. Whatever
 * the system refuses is left out. */
int plat_cores_start(StressWorker *w, int max) {
  s32 prio = 0x30;
  int n = 0;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  APT_SetAppCpuTimeLimit(80);
  for (int core = 1; core <= 3 && n < max && n < MAX_THREADS; core++) {
    w[n].core = core;
    w[n].stop = 0;
    w[n].rounds = 0;
    g_threads[n] = threadCreate(worker_entry, &w[n], 64 * 1024,
                                prio < 0x3F ? prio + 1 : 0x3F, core, false);
    if (g_threads[n])
      n++;
  }
  return n;
}

void plat_cores_stop(StressWorker *w, int n) {
  for (int i = 0; i < n; i++)
    w[i].stop = 1;
  for (int i = 0; i < n; i++) {
    threadJoin(g_threads[i], U64_MAX);
    threadFree(g_threads[i]);
    g_threads[i] = NULL;
  }
}

int main(void) {
  static char system[96];
  static PlatInfo pi = {
      .title = "Horizon",
      .cpu = "ARM11 MPCore, core 0, VFPv2: the app's own thread runs the "
             "workloads; the stress adds the cores Horizon allows",
      .cpu_short = "ARM11 MPCore core 0",
      .build = "gcc " __VERSION__ ", -march=armv6k -mtune=mpcore "
               "-mfloat-abi=hard -O2 (libctru)",
      .result_path = "sdmc:/Aurora/nintendo_stress.txt",
      .result_name = "SD:/Aurora/nintendo_stress.txt",
  };
  u32 kv;

  gfxInitDefault();
  gfxSet3D(false);
  runfast();
  g_mcu = R_SUCCEEDED(mcuHwcInit());
  g_ptm = R_SUCCEEDED(ptmuInit());
  APT_CheckNew3DS(&g_n3ds);
  g_sysm = g_n3ds && R_SUCCEEDED(ptmSysmInit());
  mode_apply();
  mkdir("sdmc:/Aurora", 0777);

  kv = osGetKernelVersion();
  snprintf(system, sizeof(system),
           "Nintendo 3DS firmware (Horizon, kernel %lu.%lu-%lu), homebrew "
           "(libctru)",
           (unsigned long)GET_VERSION_MAJOR(kv),
           (unsigned long)GET_VERSION_MINOR(kv),
           (unsigned long)GET_VERSION_REVISION(kv));
  pi.system = system;
  pi.model = g_n3ds ? "New 3DS family" : "Old 3DS family";

  stress_main(&pi);

  if (g_n3ds) {
    if (g_sysm) {
      PTMSYSM_ConfigureNew3DSCPU(0);
      ptmSysmExit();
    } else {
      osSetSpeedupEnable(false);
    }
  }
  if (g_ptm)
    ptmuExit();
  if (g_mcu)
    mcuHwcExit();
  gfxExit();
  return 0;
}
