/* Auric runtime: maps the built-ins onto AuroraOS. Drawing comes from
 * src/screen.c, compiled alongside; input, timing and sound live here so an app
 * does not pull in the Home Menu. Everything draws on the top screen (400x240,
 * 8x8 font). */
#include "aurora.h"
#include "gpu.h"
#include "i2c.h"
#include "loader.h"
#include "audio.h"
#include "ff.h"
#include "wav.h"
#include "auric_runtime.h"

/* ARM9 timers at 0x10003000: a value/reload register then a control register
 * per timer. Control bits 0-1 prescaler (/1, /64, /256, /1024), bit 2 count-up,
 * bit 7 enable. Timer 0 runs free at /1024 and timer 1 counts its overflows, a
 * 32-bit count that wraps after about 18 hours. The ARM9 timer clock is 67.03
 * MHz, so /1024 is about 65 ticks per millisecond. */
#define TIMER_BASE     0x10003000u
#define TIMER0_VAL     (*(volatile u16 *)(TIMER_BASE + 0x00))
#define TIMER0_CNT     (*(volatile u16 *)(TIMER_BASE + 0x02))
#define TIMER1_VAL     (*(volatile u16 *)(TIMER_BASE + 0x04))
#define TIMER1_CNT     (*(volatile u16 *)(TIMER_BASE + 0x06))
#define TIMER_PRESCALE_1024 3u
#define TIMER_COUNT_UP      (1u << 2)
#define TIMER_ENABLE        (1u << 7)
#define TICKS_PER_MS        65u

static int aur_timer_ready = 0;

static void aur_timer_init(void) {
  TIMER0_CNT = 0; /* stop both before reprogramming */
  TIMER1_CNT = 0;
  TIMER0_VAL = 0; /* reload value: count the full 16-bit range */
  TIMER1_VAL = 0;
  TIMER1_CNT = (u16)(TIMER_ENABLE | TIMER_COUNT_UP);
  TIMER0_CNT = (u16)(TIMER_ENABLE | TIMER_PRESCALE_1024);
  aur_timer_ready = 1;
}

static u32 aur_ticks(void) {
  u32 hi, lo, hi2;

  if (!aur_timer_ready)
    aur_timer_init();
  /* Re-read the low half if the high half moved between the two reads. */
  hi = TIMER1_VAL;
  lo = TIMER0_VAL;
  hi2 = TIMER1_VAL;
  if (hi != hi2) {
    hi = hi2;
    lo = TIMER0_VAL;
  }
  return (hi << 16) | lo;
}

int aur_millis(void) { return (int)(aur_ticks() / TICKS_PER_MS); }

/* Current text-background colour, updated by aur_clear so text drawn after a
 * clear() sits on a matching background instead of a black box. */
static Color aur_bg = {0x00, 0x00, 0x00};

static Color aur_unpack(int packed) {
  Color c;
  c.r = (u8)((packed >> 16) & 0xFF);
  c.g = (u8)((packed >> 8) & 0xFF);
  c.b = (u8)(packed & 0xFF);
  return c;
}

/* Sound plays on the ARM11's CSND voices, which read samples in place, so they
 * load into the OS's PCM pool (unused while an app runs) and are flushed to
 * physical memory. */
extern void os_cache_sync(void);
extern void os_dcache_clean_range(const void *addr, u32 len);

#define AUR_SOUNDS     32
#define AUR_MUSIC_VOL  0x5000u /* under the effects, so they carry over it */
#define AUR_SFX_VOL    0x8000u
#define AUR_MUSIC_MASK (1u << 0)
#define AUR_SFX_MASK   (AUDIO_VOICE_ALL & ~AUR_MUSIC_MASK)

typedef struct {
  u32 addr, bytes, rate, flags;
} AurSound;

static AurSound aur_sounds[AUR_SOUNDS];
static int aur_sound_count;
static u32 aur_pool_used;
static int aur_audio;          /* 0 not checked yet, 1 usable, -1 not */
static int aur_voices_started; /* anything played that might still sound */

/* Launched by the Home Menu rather than booted directly. Only then is the
 * ARM11 core, and so the GPU and sound, there to answer. */
static int aur_from_home(void) {
  volatile u32 *desc = (volatile u32 *)AURORA_RETURN_DESC_ADDR;
  return desc[0] == AURORA_RETURN_READY_MAGIC;
}

static int aur_audio_ready(void) {
  if (!aur_audio) {
    const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
    os_cache_sync();
    aur_audio = (aur_from_home() && ct->magic == AUDIO_MAGIC &&
                 ct->version >= AUDIO_VOICE_VERSION) ? 1 : -1;
  }
  return aur_audio > 0;
}

/* The ARM11 writes ack_seq, so drop the cached line to see its change. */
static void aur_inval_line(const void *p) {
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(p) : "memory");
}

/* Posts one request and waits until the core has taken it: the block holds one
 * request and present() uses it too. The bound only stops a dead core from
 * hanging the app. */
static void aur_audio_post(u32 cmd, u32 a0, u32 a1, u32 a2, u32 a3) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;

  gpu_wait_idle();
  ct->cmd = cmd;
  ct->arg0 = a0;
  ct->arg1 = a1;
  ct->arg2 = a2;
  ct->arg3 = a3;
  /* The arguments reach physical memory before the counter announcing them. */
  os_dcache_clean_range(&ct->cmd, 5u * sizeof(u32));
  ct->cmd_seq = ct->cmd_seq + 1;
  os_dcache_clean_range(&ct->cmd_seq, sizeof(u32));
  for (u32 spin = 0; spin < 2000000u; spin++) {
    aur_inval_line(&ct->ack_seq);
    if (ct->ack_seq == ct->cmd_seq)
      return;
  }
}

void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop");
}

/* Pad bits 0..9 are A/B/Select/Start/D-pad/R/L; 10 and 11 are X and Y. */
u32 get_keys(void) { return ~REG_HID_PAD & 0xFFF; }

static u32 aur_prev_keys = 0;
u32 get_keys_down(void) {
  u32 cur = get_keys();
  u32 down = cur & ~aur_prev_keys;
  aur_prev_keys = cur;
  return down;
}

/* HOME is not on the HID pad; it is reported by the MCU. Reading MCU IRQ
 * register 0x10 clears its flags; bit 2 of byte 0 signals a HOME press. */
static int aur_i2c_ready = 0;

static int aur_home_pressed(void) {
  if (!aur_i2c_ready) {
    I2C_init();
    aur_i2c_ready = 1;
  }
  u8 irq[4];
  if (!I2C_readRegBuf(I2C_DEV_MCU, 0x10, irq, sizeof(irq)))
    return 0;
  return (irq[0] & 0x04) != 0; /* bit 2 = HOME pressed */
}

/* A directly booted app has no OS to return to, so HOME is ignored. */
static void aur_home_poll_now(void) {
  if (!aur_from_home())
    return;
  if (aur_home_pressed()) {
    /* The samples sit in memory the Home Menu takes back, so nothing may still
     * be playing them when it does. */
    if (aur_voices_started)
      aur_audio_post(AUDIO_CMD_VOICE_STOP, AUDIO_VOICE_ALL, 0, 0, 0);
    ((void (*)(void))AURORA_RETURN_STUB_ADDR)(); /* restores the OS; no return */
  }
}

/* Called from every built-in. An MCU read is a full I2C transaction and a game
 * calls built-ins hundreds of times a frame, so HOME is sampled every 64 calls;
 * blocking calls poll it directly. */
static int aur_home_tick = 0;

void aur_check_home(void) {
  if (++aur_home_tick < 64)
    return;
  aur_home_tick = 0;
  aur_home_poll_now();
}

/* Immediate mode presents after every drawing call; buffered mode waits for
 * present(). */
static int aur_buffered_mode = 0;

static void aur_auto_present(void) {
  if (!aur_buffered_mode)
    screen_present_top();
}

void aur_buffered(int on) {
  aur_check_home();
  aur_buffered_mode = on ? 1 : 0;
  if (aur_buffered_mode && g_screen_blit == 0) {
    /* Present by GPU blit when launched from the Home Menu, whose ARM11 core is
     * still running; a directly booted app keeps the CPU copy. */
    if (aur_from_home() && gpu_init())
      g_screen_blit = gpu_texcopy;
  }
  screen_use_backbuffer(aur_buffered_mode);
}

void aur_present(void) {
  aur_check_home();
  screen_present_top();
}

void aur_print(const char *text, int x, int y, int color) {
  aur_check_home();
  draw_string(VRAM_TOP_LA, x, y, TOP_SCREEN_HEIGHT, text, aur_unpack(color),
              aur_bg);
  aur_auto_present();
}

void aur_print_int(int value, int x, int y, int color) {
  char buf[13];
  int i = sizeof(buf) - 1;
  unsigned int mag;
  int negative = value < 0;

  aur_check_home();
  buf[i] = '\0';
  mag = negative ? (unsigned int)(-(long)value) : (unsigned int)value;
  do {
    buf[--i] = (char)('0' + (mag % 10u));
    mag /= 10u;
  } while (mag && i > 1);
  if (negative && i > 0)
    buf[--i] = '-';

  draw_string(VRAM_TOP_LA, x, y, TOP_SCREEN_HEIGHT, &buf[i], aur_unpack(color),
              aur_bg);
  aur_auto_present();
}

void aur_clear(int color) {
  aur_check_home();
  aur_bg = aur_unpack(color);
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, aur_bg);
  aur_auto_present();
}

void aur_fill_rect(int x, int y, int w, int h, int color) {
  aur_check_home();
  draw_filled_rect(VRAM_TOP_LA, x, y, w, h, TOP_SCREEN_HEIGHT, aur_unpack(color));
  aur_auto_present();
}

int aur_keys_down(void) {
  aur_check_home();
  return (int)get_keys_down();
}

int aur_keys_held(void) {
  aur_check_home();
  return (int)get_keys();
}

/* xorshift32, seeded once from the MCU clock so runs differ. If the MCU does
 * not answer, the fixed fallback still gives a usable sequence. */
static u32 aur_rng = 0;

static void aur_seed_rng(void) {
  u8 t[3];
  aur_rng = 0x2545F491u;
  if (aur_i2c_ready == 0) {
    I2C_init();
    aur_i2c_ready = 1;
  }
  if (I2C_readRegBuf(I2C_DEV_MCU, 0x30, t, sizeof(t)))
    aur_rng ^= (u32)t[0] | ((u32)t[1] << 8) | ((u32)t[2] << 16);
  if (aur_rng == 0)
    aur_rng = 1; /* xorshift is stuck at zero */
}

int aur_rand(int n) {
  aur_check_home();
  if (n <= 1)
    return 0;
  if (aur_rng == 0)
    aur_seed_rng();
  aur_rng ^= aur_rng << 13;
  aur_rng ^= aur_rng >> 17;
  aur_rng ^= aur_rng << 5;
  return (int)((aur_rng >> 1) % (u32)n);
}

void aur_wait_key(int button) {
  /* Prime the edge detector so a button already held on entry is ignored. */
  get_keys_down();
  while (1) {
    aur_home_poll_now(); /* blocking call: poll HOME every iteration */
    if (get_keys_down() & (u32)button)
      return;
    delay(60000);
  }
}

void aur_delay(int cycles) {
  aur_check_home();
  if (cycles > 0)
    delay((u32)cycles);
}

int aur_load_sound(const char *path) {
  static FATFS fs;
  static int mounted;
  WavInfo info;
  AurSound *s;
  u32 at;

  aur_check_home();
  if (aur_sound_count >= AUR_SOUNDS || !aur_audio_ready())
    return -1;
  if (!mounted) {
    if (f_mount(&fs, "", 1) != FR_OK)
      return -1;
    mounted = 1;
  }
  at = (aur_pool_used + 3u) & ~3u;
  if (at >= AUDIO_PCM_MAX)
    return -1;
  /* Rates above 32 kHz are halved as they load: a CD-rate file then needs half
   * the pool, and the CSND output is no finer than that anyway. */
  if (wav_load(path, (u8 *)(AUDIO_PCM_ADDR + at), AUDIO_PCM_MAX - at, 1,
               &info) != WAV_OK)
    return -1;
  os_cache_sync(); /* CSND reads physical memory, not the ARM9's cache */

  s = &aur_sounds[aur_sound_count];
  s->addr = AUDIO_PCM_ADDR + at;
  s->bytes = info.samples * (info.depth / 8u);
  s->rate = info.rate;
  s->flags = (info.depth == 16) ? AUDIO_VOICE_PCM16 : 0u;
  aur_pool_used = at + s->bytes;
  return aur_sound_count++;
}

static const AurSound *aur_sound(int handle) {
  return (handle >= 0 && handle < aur_sound_count) ? &aur_sounds[handle] : 0;
}

void aur_play_sound(int handle) {
  const AurSound *s = aur_sound(handle);
  aur_check_home();
  if (!s)
    return;
  aur_audio_post(AUDIO_CMD_VOICE,
                 AUDIO_VOICE_ANY | s->flags | AUDIO_VOICE_VOL(AUR_SFX_VOL),
                 s->addr, s->bytes, s->rate);
  aur_voices_started = 1;
}

void aur_play_music(int handle) {
  const AurSound *s = aur_sound(handle);
  aur_check_home();
  if (!s)
    return;
  aur_audio_post(AUDIO_CMD_VOICE,
                 0u | AUDIO_VOICE_LOOP | s->flags | AUDIO_VOICE_VOL(AUR_MUSIC_VOL),
                 s->addr, s->bytes, s->rate);
  aur_voices_started = 1;
}

void aur_stop_music(void) {
  aur_check_home();
  if (aur_voices_started)
    aur_audio_post(AUDIO_CMD_VOICE_STOP, AUR_MUSIC_MASK, 0, 0, 0);
}

void aur_stop_sounds(void) {
  aur_check_home();
  if (aur_voices_started)
    aur_audio_post(AUDIO_CMD_VOICE_STOP, AUR_SFX_MASK, 0, 0, 0);
}
