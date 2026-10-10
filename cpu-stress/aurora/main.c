// CPU stress test for AuroraOS. The workloads run on the ARM11, the CPU
// Horizon runs apps on, as jobs AuroraOS's core runs for this app
// (AUDIO_CMD_RUN11, core 116 and later): core 0, with the L1 caches and the
// VFP on while a job runs. On a New 3DS the ARM11 is switched to 804 MHz for
// the test and back afterwards. The ARM9 draws the screens and reads the
// battery meanwhile. Build with build.py. Results: SD:/Aurora/stress.txt.
#include <aurora_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/audio.h"
#include "job11.h"
#include "job11_blob.h"
#include "stress.h"

/* The SDK runtime's cache maintenance (sdk/runtime/app_start.s). */
extern void os_cache_sync(void);

#define CLKCNT_MODE_804 5u
#define JOB_TIMEOUT_US  30000000u

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;
static Job11 *const job = (Job11 *)JOB11_PARAMS;
static uint64_t job_t0;
static int job_posted;
static int clock_changed;
static uint32_t clock_before;
static char clock_note[96];

/* The core acknowledges a command when it has finished it. */
static int core_idle(void) {
  os_cache_sync();
  return ctrl->ack_seq == ctrl->cmd_seq;
}

static int core_wait(uint32_t ms) {
  uint32_t t0 = sys_millis();
  while (!core_idle())
    if (sys_millis() - t0 > ms)
      return 0;
  return 1;
}

/* The block holds one command, and the SDK's presents use it too; each
 * present has finished by the time gfx_present() returns. */
static int core_post(uint32_t cmd, uint32_t a0, uint32_t a1) {
  if (!core_wait(2000))
    return 0;
  ctrl->cmd = cmd;
  ctrl->arg0 = a0;
  ctrl->arg1 = a1;
  os_cache_sync();
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
  return 1;
}

WorkCtx *plat_work_ctx(void) { return &job->ctx; }

/* Timed from the moment it is posted, once the core is free for it. */
void plat_work_start(uint32_t kind, uint32_t reps) {
  job->kind = kind;
  job->reps = reps;
  job->ran = 0;
  core_wait(2000);
  job_t0 = sys_micros();
  job_posted = core_post(AUDIO_CMD_RUN11, JOB11_CODE, JOB11_PARAMS);
}

uint64_t plat_work_wait(void) {
  uint64_t t;
  if (!job_posted)
    return 0;
  while (!core_idle())
    if (sys_micros() - job_t0 > JOB_TIMEOUT_US) {
      job_posted = 0;
      return 0;
    }
  t = sys_micros() - job_t0;
  job_posted = 0;
  return job->ran ? (t ? t : 1u) : 0;
}

/* The ARM11 is AuroraOS's: no other core of it is free for the stress. */
int plat_cores_start(StressWorker *w, int max) {
  (void)w;
  (void)max;
  return 0;
}

void plat_cores_stop(StressWorker *w, int n) {
  (void)w;
  (void)n;
}

uint64_t plat_ticks(void) { return sys_micros(); }
uint64_t plat_tick_hz(void) { return 1000000u; }

int plat_update(uint32_t *down) {
  static const struct {
    uint32_t sdk, test;
  } map[] = {
      {KEY_A, K_A},         {KEY_B, K_B},         {KEY_X, K_X},
      {KEY_START, K_START}, {KEY_L, K_L},         {KEY_R, K_R},
      {KEY_LEFT, K_LEFT},   {KEY_RIGHT, K_RIGHT},
  };
  uint32_t k;
  hid_scan(); /* HOME leaves from here, through leave() */
  k = hid_keys_down();
  *down = 0;
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (k & map[i].sdk)
      *down |= map[i].test;
  return 1;
}

uint8_t *plat_fb(int bottom) {
  return gfx_framebuffer(bottom ? SCREEN_BOTTOM : SCREEN_TOP);
}

void plat_present(void) { gfx_present(); }

void plat_wait(uint32_t ms) { sys_sleep(ms); }

void plat_battery(Battery *b) {
  int t = sys_battery_temperature();
  b->temp_c = t == SYS_TEMP_UNKNOWN ? TEMP_UNKNOWN : t;
  b->tenths = sys_battery_tenths();
  b->mv = sys_battery_millivolts();
  b->charging = sys_charging() ? 1 : 0;
}

void plat_date(char *out, int size) {
  SysTime t;
  if (sys_time(&t))
    snprintf(out, (size_t)size, "%04d-%02d-%02d %02d:%02d", t.year, t.month,
             t.day, t.hour, t.minute);
  else
    snprintf(out, (size_t)size, "unknown");
}

void plat_notes(char *out, int size) {
  snprintf(out, (size_t)size, "%s", clock_note);
}

const char *plat_option(void) { return 0; }
void plat_option_toggle(void) {}

/* 804 MHz on a New 3DS, as the Horizon version asks for too. */
static void clock_fast(void) {
  uint32_t status;
  if (!sys_is_new3ds()) {
    snprintf(clock_note, sizeof(clock_note), "Old 3DS: 268 MHz, L2 cache off");
    return;
  }
  ctrl->n3ds_status = AUDIO_N3DS_PENDING;
  if (!core_post(AUDIO_CMD_N3DS, CLKCNT_MODE_804, 0) || !core_wait(3000)) {
    snprintf(clock_note, sizeof(clock_note), "804 MHz: no answer, L2 off");
    return;
  }
  status = ctrl->n3ds_status;
  clock_before = (ctrl->n3ds_before >> 16) & 7u;
  if (status == AUDIO_N3DS_APPLIED) {
    clock_changed = 1;
    snprintf(clock_note, sizeof(clock_note), "804 MHz, L2 cache off");
  } else if (status == AUDIO_N3DS_ALREADY) {
    snprintf(clock_note, sizeof(clock_note), "804 MHz (already), L2 off");
  } else {
    snprintf(clock_note, sizeof(clock_note), "804 MHz refused (%u), L2 off",
             (unsigned)status);
  }
}

/* Also on HOME: a job the core is still running finishes first, then the
 * clock goes back to what it was. */
static void leave(void) {
  if (job_posted)
    plat_work_wait();
  if (clock_changed && core_post(AUDIO_CMD_N3DS, clock_before, 0))
    core_wait(3000);
  clock_changed = 0;
}

static void refuse(const char *why) {
  printf("CPU stress test\n\n%s\n\nPress START to quit.\n", why);
  hid_wait(KEY_START);
}

int main(void) {
  static char system[96];
  static PlatInfo pi = {
      .title = "AuroraOS",
      .cpu = "ARM11 MPCore, core 0, VFPv2: AuroraOS's core runs the "
             "workloads as jobs, with the L1 caches on (L2 off); the ARM9 "
             "draws",
      .cpu_short = "ARM11 MPCore core 0",
      .build = JOB11_BUILD,
      .result_path = "/Aurora/stress.txt",
      .result_name = "SD:/Aurora/stress.txt",
  };
  if (!app_from_home()) {
    refuse("Start it from the Home Menu: the ARM11 runs the test.");
    return 0;
  }
  os_cache_sync();
  if (ctrl->version < AUDIO_RUN11_VERSION) {
    char why[96];
    snprintf(why, sizeof(why),
             "This needs AuroraOS with ARM11 core %u or later\n(running: %u). "
             "Copy the new AURORAOS.BIN.",
             (unsigned)AUDIO_RUN11_VERSION, (unsigned)ctrl->version);
    refuse(why);
    return 0;
  }
  memcpy((void *)JOB11_CODE, job11_blob, sizeof(job11_blob));
  memset(job, 0, sizeof(*job));
  os_cache_sync();
  atexit(leave);
  clock_fast();

  snprintf(system, sizeof(system),
           "AuroraOS (ARM11 core %u), native C app (C SDK %s)",
           (unsigned)ctrl->version, AURORA_SDK_VERSION);
  pi.system = system;
  pi.model = sys_is_new3ds() ? "New 3DS family" : "Old 3DS family";
  stress_main(&pi);
  return 0;
}
