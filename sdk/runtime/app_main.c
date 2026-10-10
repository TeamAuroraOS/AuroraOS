/* Start-up, the frame loop, HOME and leaving. */
#include "app_internal.h"
#include "audio.h"
#include "container.h"
#include "crash.h"
#include "gpu.h"
#include "i2c.h"
#include "loader.h"
#include "timer.h"
#include "user.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int main(int argc, char **argv);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
/* The app's file name without ".bin", written into the app header by
 * aurcc.py: the fallback for app_path() when the OS did not say. */
extern const char _app_default_name[];
extern void os_cache_sync(void);
extern void os_dcache_clean_range(const void *addr, u32 len);

bool g_app_leaving;

static u32 g_entry_mode; /* CPSR at entry, for leaving from the crash screen */
static bool g_quit;
static char g_path[AURORA_LAUNCH_PATH_MAX + 8];
static char g_dir[AURORA_LAUNCH_PATH_MAX + 32];

void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop");
}

/* Only an app the Home Menu launched has the return stub, and the ARM11 core
 * (GPU, sound, touch) running behind it. */
bool app_from_home(void) {
  volatile u32 *desc = (volatile u32 *)AURORA_RETURN_DESC_ADDR;
  return desc[0] == AURORA_RETURN_READY_MAGIC;
}

bool app_core(void) {
  static int state; /* 0 not asked yet, 1 there, -1 not */
  if (!state) {
    const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
    os_cache_sync();
    state = app_from_home() && ct->magic == AUDIO_MAGIC ? 1 : -1;
  }
  return state > 0;
}

uint32_t audio_version(void) {
  const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
  os_cache_sync();
  return ct->version;
}

int audio_alive(void) { return app_core(); }

static void inval_line(const volatile void *p) {
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(p) : "memory");
}

/* The block holds one request, and a GPU present uses it too. The bound only
 * stops a dead core from hanging the app. */
void app_core_post(u32 cmd, u32 a0, u32 a1, u32 a2, u32 a3) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;

  if (!app_core() || app_net_busy())
    return;
  gpu_wait_idle();
  ct->cmd = cmd;
  ct->arg0 = a0;
  ct->arg1 = a1;
  ct->arg2 = a2;
  ct->arg3 = a3;
  /* The arguments reach memory before the counter that announces them. */
  os_dcache_clean_range((const void *)&ct->cmd, 5u * sizeof(u32));
  ct->cmd_seq = ct->cmd_seq + 1;
  os_dcache_clean_range((const void *)&ct->cmd_seq, sizeof(u32));
  for (u32 spin = 0; spin < 2000000u; spin++) {
    inval_line(&ct->ack_seq);
    if (ct->ack_seq == ct->cmd_seq)
      return;
  }
}

void audio_error_beep(void) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  if (!app_from_home() || ct->magic != AUDIO_MAGIC)
    return;
  ct->cmd = AUDIO_CMD_ERROR;
  os_dcache_clean_range((const void *)&ct->cmd, sizeof(u32));
  ct->cmd_seq = ct->cmd_seq + 1;
  os_dcache_clean_range((const void *)&ct->cmd_seq, sizeof(u32));
}

/* HOME is not on the HID pad: the MCU reports it in its interrupt flags,
 * which reading register 0x10 also clears. Bit 2 of the first byte is a HOME
 * press. */
bool app_home_pressed(void) {
  u8 irq[4];
  if (!I2C_readRegBuf(I2C_DEV_MCU, 0x10, irq, sizeof(irq)))
    return false;
  return (irq[0] & 0x04) != 0;
}

void app_poll_home(void) {
  if (g_app_leaving || !app_from_home())
    return;
  if (app_home_pressed())
    exit(0);
}

/* exit() has run the atexit functions and closed the stdio files. */
void app_leave(int code) {
  (void)code;
  g_app_leaving = true;
  app_net_leave();
  app_fs_close_all();
  app_snd_leave();
  gpu_wait_idle();
  /* Presents switch the panels between framebuffers A, B and C; the Home
   * Menu starts from A, as an app found it. */
  if (app_core())
    gpu_show_a();
  if (app_from_home())
    ((void (*)(void))AURORA_RETURN_STUB_ADDR)(); /* restarts the Home Menu */
  /* Booted directly: nothing to go back to, so the last frame stays. */
  gfx_present();
  for (;;)
    __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" ::"r"(0)); /* wait */
}

/* From the crash screen's countdown, in the exception's CPU mode: back in the
 * mode the app started in, the return stub restarts the Home Menu. */
static void crash_poll(void) {
  if (!app_from_home() || !app_home_pressed())
    return;
  app_snd_leave();
  __asm__ volatile("msr cpsr_c, %0\n\t"
                   "bx %1"
                   :
                   : "r"((g_entry_mode & 0x1Fu) | 0xC0u),
                     "r"(AURORA_RETURN_STUB_ADDR)
                   : "memory");
  __builtin_unreachable();
}

static void default_path(const char *folder) {
  strcpy(g_path, folder);
  strncat(g_path, _app_default_name, AURORA_LAUNCH_PATH_MAX - 20);
  strcat(g_path, ".bin");
}

/* "Aurora/Apps/C/X.bin" (Home Menu) and "0:/Aurora/Apps/C/X.bin" (Files, the
 * Terminal) both become "/Aurora/Apps/C/X.bin". There is no block for an app
 * booted directly by the launcher, or an AUR1 build run by an OS from before
 * C apps, which lists only /Aurora/Apps: the app is taken to be there if it
 * is, or else in /Aurora/Apps/C. The data folder is the one the OS makes,
 * "/Aurora/Apps/C/X" wherever the app is. */
static void launch_read(void) {
  volatile u32 *info = (volatile u32 *)AURORA_LAUNCH_INFO_ADDR;
  const volatile char *src = (const volatile char *)(info + 1);
  static FILINFO fno;
  u32 n = 0;

  if (info[0] == AURORA_LAUNCH_MAGIC) {
    if (src[0] == '0' && src[1] == ':')
      src += 2;
    if (src[0] != '/')
      g_path[n++] = '/';
    while (*src && n < AURORA_LAUNCH_PATH_MAX)
      g_path[n++] = *src++;
    g_path[n] = '\0';
    info[0] = 0; /* used once; the next launch writes it again */
  } else {
    default_path("/Aurora/Apps/");
    if (!app_fs_mount() || f_stat(g_path, &fno) != FR_OK)
      default_path("/Aurora/Apps/C/");
  }
  g_dir[0] = '/';
  if (!aurora_c_data_dir(g_path, g_dir + 1, sizeof(g_dir) - 1))
    strcpy(g_dir, "/" AURORA_C_APPS_DIR);
}

const char *app_path(void) { return g_path; }
const char *app_dir(void) { return g_dir; }

const u8 *app_user_dat(void) {
  static u8 dat[USER_DAT_SIZE];
  static int state; /* 0 not read yet, 1 valid, -1 not */
  static FIL f;
  UINT got = 0;

  if (!state) {
    state = -1;
    if (app_fs_mount() && f_open(&f, "0:/" USER_DAT_PATH, FA_READ) == FR_OK) {
      if (f_read(&f, dat, sizeof(dat), &got) == FR_OK &&
          got >= 12 && memcmp(dat, USER_DAT_MAGIC, 4) == 0)
        state = 1;
      f_close(&f);
    }
  }
  return state > 0 ? dat : NULL;
}

/* printf() keeps a partial line until a newline; anything shown on screen
 * should include it. A weak reference, so an app that never uses stdio does
 * not pull it in. */
extern int fflush(FILE *f) __attribute__((weak));

void app_flush_stdout(void) {
  if (fflush)
    fflush(stdout);
}

static u32 g_frame_ticks; /* one 60 Hz frame in timer ticks */
static u32 g_last_frame;
static u32 g_fps_t0, g_fps_frames;
static int g_fps;

bool app_loop(void) {
  u32 now;
  u32 shown;

  app_flush_stdout();
  shown = app_present();
  if (!g_frame_ticks)
    g_frame_ticks = (timer_hz() + 30u) / 60u;

  /* A present that waits for the vertical blank has paced the frame already;
   * without one, the timer does. */
  now = timer_ticks();
  if (!shown || !app_core() || (gpu_vsynced() & shown) != shown) {
    while (now - g_last_frame < g_frame_ticks)
      now = timer_ticks();
  }
  g_last_frame = now;

  g_fps_frames++;
  if (now - g_fps_t0 >= timer_hz()) {
    g_fps = (int)g_fps_frames;
    g_fps_frames = 0;
    g_fps_t0 = now;
  }

  hid_scan();
  return !g_quit;
}

void app_quit(void) { g_quit = true; }

int app_fps(void) { return g_fps; }

void app_entry(void) {
  static char *argv[2];

  __asm__ volatile("mrs %0, cpsr" : "=r"(g_entry_mode));
  crash_init(); /* the vectors still point into the OS this app replaced */
  g_crash_poll = crash_poll;
  timer_ready();
  I2C_init();
  launch_read();
  app_hid_init();
  app_gfx_init();
  g_last_frame = g_fps_t0 = timer_ticks();

  for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
    (*f)();

  argv[0] = g_path;
  exit(main(1, argv));
}
