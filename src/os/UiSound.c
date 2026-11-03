#include "uisound.h"
#include "assets.h"
#include "audio.h"
#include "ff.h"
#include "glb.h"
#include "timer.h"
#include "user.h"
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

void ui_sound_config(u8 off) { sound_off = off; }

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
    menu_samples[i] = load(ids[i], addr, SLOT_MUSIC, &menu_rate[i]);
    addr += (menu_samples[i] * 2u + 31u) & ~31u;
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

/* Reads the packed song in one go, the only part that needs the card, since
 * every other user of the card mounts and unmounts around its own work. */
static void home_fetch(void) {
  u32 n = 0;
  if (f_mount(&fs, "", 1) == FR_OK)
    n = asset_sound_open(ASSET_SOUND_HOME, (void *)HOME_PACKED, HOME_PACKED_MAX,
                         (int16_t *)HOME_PCM, HOME_PCM_MAX / 2u, &home_stream,
                         &home_rate);
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

void ui_sound_home_tick(void) {
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
    if (core_free()) {
      audio_voice_play(VOICE_MUSIC, HOME_PCM, home_samples, home_rate,
                       AUDIO_VOICE_LOOP | AUDIO_VOICE_VOL(HOME_VOLUME));
      home_playing = 1;
    }
  }
}

static void home_stop(void) {
  if (home_playing && core_free()) {
    audio_voices_stop_mask(1u << VOICE_MUSIC);
    home_playing = 0;
  }
}

void ui_sound_home_leave(void) {
  home_shown = 0;
  hold_ticks = 0;
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
