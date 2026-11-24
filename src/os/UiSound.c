#include "uisound.h"
#include "assets.h"
#include "audio.h"
#include "ff.h"
#include "glb.h"
#include "timer.h"
#include "user.h"
#include "wav.h"
#include "wifi.h"

/* 256 KB (4 s) for each short sound and for the three menu sounds together,
 * 1.75 MB (28 s) for the music. */
#define SLOT_STARTUP UI_SOUND_ADDR
#define SLOT_CHIME   (UI_SOUND_ADDR + 0x40000u)
#define SLOT_MENU    (UI_SOUND_ADDR + 0x80000u)
#define SLOT_MUSIC   (UI_SOUND_ADDR + 0xC0000u)
#define SLOT_END     0x28000000u

/* The Home Menu's song needs 2.6 MB decoded and 0.74 MB packed, which FCRAM
 * only has free in 3D Model's file buffer, idle whenever the menu is showing.
 * This is room for 80 s. */
#define HOME_PCM        GLB_FILE_ADDR
#define HOME_PCM_MAX    0x00500000u
#define HOME_PACKED     (HOME_PCM + HOME_PCM_MAX)
#define HOME_PACKED_MAX 0x00180000u
typedef char home_song_fits[
    HOME_PACKED + HOME_PACKED_MAX <= GLB_FILE_ADDR + GLB_FILE_MAX ? 1 : -1];

/* 70% of full scale: the song peaks at full scale and the menu sounds play over
 * it at full volume. */
#define HOME_VOLUME 0x5999u

/* Frames per decode between looks at the clock, and the time a pass of the
 * menu's loop may spend on them. */
#define HOME_SLICE_FRAMES 32u
#define HOME_SLICE_MS     1u

#define VOICE_MUSIC   0u
#define VOICE_CHIME   1u
#define VOICE_STARTUP 2u
#define VOICE_MENU    3u
/* Moves take turns on voices 4 and 5, so key repeat does not cut each one
 * off. */
#define VOICE_MOVE    4u
#define VOICE_BANNER  6u
#define BANNER_GAP_MS 400u

enum { MENU_SELECT, MENU_BACK, MENU_MOVE, MENU_COUNT };

static u8 sound_off;
static int shop_playing;
static u32 chime_samples, chime_rate;
static u32 menu_addr[MENU_COUNT], menu_samples[MENU_COUNT];
static u32 menu_rate[MENU_COUNT];
static u32 move_turn;
static FATFS fs;

typedef enum { HOME_UNLOADED, HOME_DECODING, HOME_READY, HOME_FAILED } HomeState;
static HomeState home_state;
static AssetSoundStream home_stream;
static u32 home_samples, home_rate;
static int home_shown, home_playing;
static u32 hold_from, hold_ticks; /* how long the startup sound still has */
static u32 home_from, home_pos;
static u32 home_resume;
static u32 tail_ticks;
static int banner_playing;

static char theme_bgm[128];
static char theme_sfx[MENU_COUNT][128];

static void copy_path(char *dst, const char *src, unsigned max) {
  unsigned i = 0;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  while (src[i] && i + 1 < max) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

void ui_sound_config(u8 off) { sound_off = off; }

void ui_sound_set_theme(const char *bgm_path, const char *select_path,
                        const char *back_path, const char *move_path) {
  copy_path(theme_bgm, bgm_path, sizeof(theme_bgm));
  copy_path(theme_sfx[MENU_SELECT], select_path, sizeof(theme_sfx[MENU_SELECT]));
  copy_path(theme_sfx[MENU_BACK], back_path, sizeof(theme_sfx[MENU_BACK]));
  copy_path(theme_sfx[MENU_MOVE], move_path, sizeof(theme_sfx[MENU_MOVE]));
}

/* Loads a custom WAV as signed 16-bit mono into [addr, end); returns sample count
 * or 0 on failure. 8-bit WAVs are expanded to 16-bit in place. */
static u32 load_wav_16(const char *path, u32 addr, u32 end, u32 *rate) {
  WavInfo wi;
  u32 max_bytes = end > addr ? (end - addr) : 0;
  if (!path || !path[0] || max_bytes < 4u)
    return 0;
  /* Reserve half when depth might be 8-bit? No, load up to max_bytes/2 if 8-bit
   * by checking after load: wav_load caps at `max`, so we pass max_bytes and
   * clamp samples to max_bytes/2 before expanding 8->16. */
  if (wav_load(path, (u8 *)addr, max_bytes, 1, &wi) != WAV_OK || !wi.samples)
    return 0;
  if (wi.depth == 8u) {
    u32 n = wi.samples;
    if (n > max_bytes / 2u)
      n = max_bytes / 2u;
    const s8 *src = (const s8 *)addr;
    s16 *dst = (s16 *)addr;
    for (u32 i = n; i > 0; i--)
      dst[i - 1u] = (s16)((int)src[i - 1u] << 8);
    wi.samples = n;
  }
  *rate = wi.rate;
  return wi.samples;
}

/* A Wi-Fi command keeps the core from taking any other request. */
static int core_free(void) { return audio_ready() && !wifi_direct_active(); }

static u32 load(u32 id, u32 addr, u32 end, u32 *rate) {
  return asset_sound_read(id, (int16_t *)addr, (end - addr) / 2u, rate);
}

static void menu_load(void) {
  static const u32 ids[MENU_COUNT] = {ASSET_SOUND_SELECT, ASSET_SOUND_BACK,
                                      ASSET_SOUND_MOVE};
  u32 addr = SLOT_MENU;
  for (int i = 0; i < MENU_COUNT; i++) {
    menu_addr[i] = addr;
    menu_samples[i] = 0;
    if (theme_sfx[i][0])
      menu_samples[i] = load_wav_16(theme_sfx[i], addr, SLOT_MUSIC, &menu_rate[i]);
    if (!menu_samples[i])
      menu_samples[i] = load(ids[i], addr, SLOT_MUSIC, &menu_rate[i]);
    addr += (menu_samples[i] * 2u + 31u) & ~31u;
  }
}

static void home_stop(void);

void ui_sound_reload_theme(void) {
  home_stop();
  home_resume = 0;
  home_state = HOME_UNLOADED;
  if (f_mount(&fs, "", 1) == FR_OK) {
    menu_load();
    f_mount(NULL, "", 0);
  }
}

void ui_sound_boot(int startup) {
  u32 n = 0, rate = 0;
  for (int i = 0; i < MENU_COUNT; i++)
    menu_samples[i] = 0;
  if (f_mount(&fs, "", 1) == FR_OK) {
    if (startup && !(sound_off & USER_SOUND_STARTUP_OFF) && core_free())
      n = load(ASSET_SOUND_STARTUP, SLOT_STARTUP, SLOT_CHIME, &rate);
    if (n) {
      audio_voice_play(VOICE_STARTUP, SLOT_STARTUP, n, rate, 0);
      hold_from = timer_ticks();
      hold_ticks = (u32)(((u64)n * timer_hz()) / rate);
    }
    menu_load();
  }
  f_mount(NULL, "", 0);
  home_shown = 1;
}

/* Reads the theme's custom bgm.wav if present, otherwise the packed built-in
 * Home Menu song from assets.pak. */
static void home_fetch(void) {
  u32 n = 0;
  if (f_mount(&fs, "", 1) == FR_OK) {
    if (theme_bgm[0]) {
      n = load_wav_16(theme_bgm, HOME_PCM, HOME_PCM + HOME_PCM_MAX, &home_rate);
      if (n) {
        f_mount(NULL, "", 0);
        home_samples = n;
        home_state = HOME_READY;
        return;
      }
    }
    n = asset_sound_open(ASSET_SOUND_HOME, (void *)HOME_PACKED, HOME_PACKED_MAX,
                         (int16_t *)HOME_PCM, HOME_PCM_MAX / 2u, &home_stream,
                         &home_rate);
  }
  f_mount(NULL, "", 0);
  home_samples = n;
  home_state = n ? HOME_DECODING : HOME_FAILED;
}

static void home_decode(void) {
  static u32 slice_ticks;
  u32 t0 = timer_ticks();
  if (!slice_ticks)
    slice_ticks = HOME_SLICE_MS * timer_hz() / 1000u;
  do {
    if (!asset_sound_decode(&home_stream, HOME_SLICE_FRAMES)) {
      home_state = HOME_FAILED;
      return;
    }
  } while (home_stream.done < home_stream.total &&
           timer_ticks() - t0 < slice_ticks);
  if (home_stream.done >= home_stream.total)
    home_state = HOME_READY;
}

static void home_tail_tick(void);

void ui_sound_home_tick(void) {
  home_tail_tick();
  if (!home_shown || home_playing || (sound_off & USER_SOUND_HOME_OFF))
    return;
  if (home_state == HOME_UNLOADED) {
    home_fetch();
  } else if (home_state == HOME_DECODING) {
    home_decode();
  } else if (home_state == HOME_READY) {
    if (hold_ticks) {
      if (timer_ticks() - hold_from < hold_ticks)
        return;
      hold_ticks = 0;
    }
    if (!core_free())
      return;
    if (home_resume && home_resume < home_samples) {
      u32 left = home_samples - home_resume;
      audio_voice_play(VOICE_MUSIC, HOME_PCM + home_resume * 2u, left, home_rate,
                       AUDIO_VOICE_VOL(HOME_VOLUME));
      home_from = timer_ticks();
      home_pos = home_resume;
      tail_ticks = (u32)(((u64)left * timer_hz()) / home_rate);
      home_resume = 0;
      home_playing = 2;
      return;
    }
    home_resume = 0;
    audio_voice_play(VOICE_MUSIC, HOME_PCM, home_samples, home_rate,
                     AUDIO_VOICE_LOOP | AUDIO_VOICE_VOL(HOME_VOLUME));
    home_from = timer_ticks();
    home_pos = 0;
    home_playing = 1;
  }
}

static void home_tail_tick(void) {
  if (home_playing == 2 && timer_ticks() - home_from >= tail_ticks && core_free()) {
    audio_voice_play(VOICE_MUSIC, HOME_PCM, home_samples, home_rate,
                     AUDIO_VOICE_LOOP | AUDIO_VOICE_VOL(HOME_VOLUME));
    home_from = timer_ticks();
    home_pos = 0;
    home_playing = 1;
  }
}

static u32 home_now(void) {
  u64 done = ((u64)(timer_ticks() - home_from) * home_rate) / timer_hz();
  return home_samples ? (u32)((home_pos + done) % home_samples) : 0;
}

static void home_stop(void) {
  if (home_playing && core_free()) {
    audio_voices_stop_mask(1u << VOICE_MUSIC);
    home_playing = 0;
  }
}

void ui_sound_banner_play(u32 addr, u32 samples, u32 rate) {
  if ((sound_off & USER_SOUND_MENU_OFF) || !samples || !rate || !core_free())
    return;
  if (home_playing) {
    home_resume = home_now();
    home_stop();
  }
  audio_voice_play(VOICE_BANNER, addr, samples, rate, 0);
  banner_playing = 1;
  hold_from = timer_ticks();
  hold_ticks = (u32)(((u64)samples * timer_hz()) / rate) +
               BANNER_GAP_MS * (timer_hz() / 1000u);
}

void ui_sound_banner_stop(void) {
  if (!banner_playing)
    return;
  if (core_free()) {
    audio_voices_stop_mask(1u << VOICE_BANNER);
    banner_playing = 0;
  }
  hold_from = timer_ticks();
  hold_ticks = BANNER_GAP_MS * (timer_hz() / 1000u);
}

void ui_sound_home_leave(void) {
  ui_sound_banner_stop();
  home_shown = 0;
  hold_ticks = 0;
  home_resume = 0;
  home_stop();
}

void ui_sound_home_enter(void) { home_shown = 1; }

void ui_sound_home_lost(void) { home_state = HOME_UNLOADED; }

void ui_sound_lid_closed(void) { home_stop(); }

static void menu_play(int i, u32 voice) {
  if (!(sound_off & USER_SOUND_MENU_OFF) && menu_samples[i] && core_free())
    audio_voice_play(voice, menu_addr[i], menu_samples[i], menu_rate[i], 0);
}

void ui_sound_select(void) { menu_play(MENU_SELECT, VOICE_MENU); }

void ui_sound_back(void) { menu_play(MENU_BACK, VOICE_MENU); }

void ui_sound_move(void) {
  move_turn ^= 1u;
  menu_play(MENU_MOVE, VOICE_MOVE + move_turn);
}

void ui_sound_close(u32 keys) {
  if (keys & BUTTON_B)
    ui_sound_back();
  else
    ui_sound_select();
}

void ui_sound_shop_start(void) {
  u32 n, rate = 0;
  chime_samples = 0;
  if ((sound_off & USER_SOUND_SHOP_OFF) || !core_free())
    return;
  chime_samples = load(ASSET_SOUND_DL_DONE, SLOT_CHIME, SLOT_MENU, &chime_rate);
  n = load(ASSET_SOUND_SHOP, SLOT_MUSIC, SLOT_END, &rate);
  if (n) {
    audio_voice_play(VOICE_MUSIC, SLOT_MUSIC, n, rate, AUDIO_VOICE_LOOP);
    shop_playing = 1;
  }
}

void ui_sound_shop_chime(void) {
  if (chime_samples && core_free()) {
    audio_voice_play(VOICE_CHIME, SLOT_CHIME, chime_samples, chime_rate, 0);
    shop_playing = 1;
  }
}

/* With the core still in a Wi-Fi command the music plays on, until the next
 * visit to aShop or a boot stops it. Only aShop's own voices stop, so the
 * menu sound of leaving it plays out. */
void ui_sound_shop_stop(void) {
  chime_samples = 0;
  if (shop_playing && core_free()) {
    audio_voices_stop_mask((1u << VOICE_MUSIC) | (1u << VOICE_CHIME));
    shop_playing = 0;
  }
}
