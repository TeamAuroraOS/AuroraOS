/* Sounds play on the ARM11 core's CSND voices, which read the samples in
 * place: they load into the OS's PCM pool (free while an app runs) and are
 * flushed from the ARM9 cache. Voice 0 carries the music; AUDIO_VOICE_ANY
 * picks a free one of voices 1..7 for each effect. */
#include "app_internal.h"
#include "audio.h"
#include "wav.h"

extern void os_cache_sync(void);

#define SOUNDS      64
#define VOL_FULL    0x8000u
#define MUSIC_MASK  (1u << 0)
#define EFFECT_MASK (AUDIO_VOICE_ALL & ~MUSIC_MASK)
/* Under the effects, so they carry over it. */
#define MUSIC_VOLUME 62

typedef struct {
  u32 addr, bytes, rate, flags;
} Sound;

static Sound g_sounds[SOUNDS];
static int g_count;
static u32 g_used;
static bool g_started; /* something may still be playing */

bool snd_available(void) {
  static int state; /* 0 not asked yet, 1 yes, -1 no */
  if (!state)
    state = app_core() && audio_version() >= AUDIO_VOICE_VERSION ? 1 : -1;
  return state > 0;
}

int snd_load(const char *path) {
  char fpath[FS_NAME_MAX + 8];
  WavInfo info;
  Sound *s;
  u32 at;

  if (g_count >= SOUNDS || !snd_available() || !app_fs_mount() ||
      !app_fs_path(path, fpath, sizeof(fpath)))
    return -1;
  at = (g_used + 3u) & ~3u;
  if (at >= AUDIO_PCM_MAX)
    return -1;
  /* Above 32 kHz a file loads at half its rate: half the pool, and the CSND
   * output is no finer than that anyway. */
  if (wav_load(fpath, (u8 *)(AUDIO_PCM_ADDR + at), AUDIO_PCM_MAX - at, 1,
               &info) != WAV_OK)
    return -1;
  os_cache_sync();

  s = &g_sounds[g_count];
  s->addr = AUDIO_PCM_ADDR + at;
  s->bytes = info.samples * (info.depth / 8u);
  s->rate = info.rate;
  s->flags = info.depth == 16 ? AUDIO_VOICE_PCM16 : 0u;
  g_used = at + s->bytes;
  return g_count++;
}

static const Sound *sound(int h) {
  return h >= 0 && h < g_count ? &g_sounds[h] : NULL;
}

static u32 volume(int v) {
  if (v < 0)
    v = 0;
  if (v > 100)
    v = 100;
  return (u32)v * VOL_FULL / 100u;
}

static void play(int h, u32 voice, int vol) {
  const Sound *s = sound(h);
  if (!s)
    return;
  app_core_post(AUDIO_CMD_VOICE, voice | s->flags | AUDIO_VOICE_VOL(volume(vol)),
                s->addr, s->bytes, s->rate);
  g_started = true;
}

void snd_play_volume(int h, int vol) { play(h, AUDIO_VOICE_ANY, vol); }
void snd_play(int h) { snd_play_volume(h, 100); }
void snd_music_volume(int h, int vol) { play(h, 0u | AUDIO_VOICE_LOOP, vol); }
void snd_music(int h) { snd_music_volume(h, MUSIC_VOLUME); }

static void stop(u32 mask) {
  if (g_started)
    app_core_post(AUDIO_CMD_VOICE_STOP, mask, 0, 0, 0);
}

void snd_stop_music(void) { stop(MUSIC_MASK); }
void snd_stop_sounds(void) { stop(EFFECT_MASK); }

void snd_unload_all(void) {
  stop(AUDIO_VOICE_ALL);
  g_started = false;
  g_count = 0;
  g_used = 0;
}

/* The samples sit in memory the Home Menu takes back. */
void app_snd_leave(void) {
  stop(AUDIO_VOICE_ALL);
  g_started = false;
}
