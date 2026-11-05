// VideoTest: plays test clips from SD:/Aurora/Video and reports how fast they ran,
// on screen and in SD:/Aurora/Video/results.txt. It tests the engine with no network
// in the way, so its numbers are the decoder's own.
//
// A clip is <name>.v.mp4, with the sound in audio.a.mp4 beside it.
// Up/Down: choose   A: play   During a clip: A pause, L/R -5 s/+5 s, B stop
#include <aurora_app.h>

#include <stdio.h>
#include <string.h>

#define DIR "/Aurora/Video"
#define MAX_CLIPS 16

static char clips[MAX_CLIPS][64];
static int n_clips, sel;
static char last[128];

static void scan(void) {
  FsDir *d = fs_dir_open(DIR);
  FsEntry e;
  n_clips = 0;
  if (!d)
    return;
  while (n_clips < MAX_CLIPS && fs_dir_read(d, &e)) {
    size_t n = strlen(e.name);
    if (n > 6 && n < 64 && !strcmp(e.name + n - 6, ".v.mp4"))
      strcpy(clips[n_clips++], e.name);
  }
  fs_dir_close(d);
  // Directory order is whatever the card has; the sizes read better in order.
  for (int i = 0; i < n_clips; i++)
    for (int k = i + 1; k < n_clips; k++)
      if (strcmp(clips[k], clips[i]) < 0) {
        char t[64];
        strcpy(t, clips[i]);
        strcpy(clips[i], clips[k]);
        strcpy(clips[k], t);
      }
}

static void menu(void) {
  gfx_clear(SCREEN_TOP, RGB(12, 14, 20));
  gfx_print(SCREEN_TOP, 16, 16, FONT_BOLD, COLOR_WHITE, "Video test");
  gfx_print(SCREEN_TOP, 16, 44, FONT_SMALL, COLOR_GRAY, "%d cores can decode here; limit %dp",
            video_cores(), video_height_limit());
  if (last[0])
    gfx_print(SCREEN_TOP, 16, 70, FONT_SMALL, COLOR_WHITE, "%s", last);
  gfx_clear(SCREEN_BOTTOM, RGB(24, 26, 32));
  if (!n_clips)
    gfx_print(SCREEN_BOTTOM, 16, 16, FONT_REGULAR, COLOR_WHITE, "No clips in %s", DIR);
  for (int i = 0; i < n_clips; i++)
    gfx_print(SCREEN_BOTTOM, 16, 12 + i * 22, FONT_REGULAR,
              i == sel ? COLOR_AURORA : COLOR_WHITE, "%s", clips[i]);
  gfx_print(SCREEN_BOTTOM, 16, 216, FONT_SMALL, COLOR_GRAY, "A: play   B or HOME: quit");
}

static void stats_screen(const VideoStats *s) {
  gfx_clear(SCREEN_BOTTOM, RGB(24, 26, 32));
  gfx_print(SCREEN_BOTTOM, 12, 8, FONT_BOLD, COLOR_WHITE, "%ux%u on %d cores  %s",
            (unsigned)s->width, (unsigned)s->height, s->cores,
            s->buffering ? "(buffering)" : s->playing ? "" : "(paused)");
  gfx_print(SCREEN_BOTTOM, 12, 36, FONT_REGULAR, COLOR_WHITE, "%u.%u fps",
            (unsigned)(s->fps_x10 / 10u), (unsigned)(s->fps_x10 % 10u));
  gfx_print(SCREEN_BOTTOM, 12, 60, FONT_SMALL, COLOR_WHITE, "at %d.%d s, %d.%d s buffered",
            (int)(s->clock_ms / 1000), (int)(s->clock_ms / 100 % 10),
            (int)(s->buffered_ms / 1000), (int)(s->buffered_ms / 100 % 10));
  gfx_print(SCREEN_BOTTOM, 12, 80, FONT_SMALL, COLOR_WHITE, "decoded %u  shown %u  dropped %u",
            (unsigned)s->decoded, (unsigned)s->shown, (unsigned)s->dropped);
  gfx_print(SCREEN_BOTTOM, 12, 100, FONT_SMALL, COLOR_WHITE, "convert %u us/frame  sound gaps %u",
            s->shown ? (unsigned)(s->convert_us / s->shown) : 0u, (unsigned)s->underruns);
  gfx_print(SCREEN_BOTTOM, 12, 216, FONT_SMALL, COLOR_GRAY,
            "A: pause   L/R: -5/+5 s   B: stop");
}

static void result(const char *clip, const VideoStats *s) {
  FILE *f;
  snprintf(last, sizeof(last), "%s: %u.%u fps, %u shown, %u dropped, %d cores", clip,
           (unsigned)(s->fps_x10 / 10u), (unsigned)(s->fps_x10 % 10u), (unsigned)s->shown,
           (unsigned)s->dropped, s->cores);
  f = fopen(DIR "/results.txt", "a");
  if (!f)
    return;
  fprintf(f, "%s | %ux%u | %d cores | %u.%u fps | decoded %u shown %u dropped %u | "
             "convert %u us | sound gaps %u | reached %d ms | %s\n",
          clip, (unsigned)s->width, (unsigned)s->height, s->cores,
          (unsigned)(s->fps_x10 / 10u), (unsigned)(s->fps_x10 % 10u), (unsigned)s->decoded,
          (unsigned)s->shown, (unsigned)s->dropped,
          s->shown ? (unsigned)(s->convert_us / s->shown) : 0u, (unsigned)s->underruns,
          (int)s->clock_ms, sys_is_new3ds() ? "New 3DS" : "Old 3DS");
  fclose(f);
}

static void play(const char *clip) {
  static char path[96];
  VideoStats s;
  uint32_t drawn = 0;
  bool paused = false, alive = true;

  snprintf(path, sizeof(path), DIR "/%s", clip);
  if (!video_open_files(path, DIR "/audio.a.mp4")) {
    snprintf(last, sizeof(last), "%s: %s", clip, video_error());
    return;
  }
  gfx_clear(SCREEN_TOP, COLOR_BLACK);
  while (alive && app_loop()) {
    uint32_t k = hid_keys_down();
    alive = video_pump();
    video_stats(&s);
    if (k & KEY_B)
      alive = false;
    if (k & KEY_A) {
      paused = !paused;
      video_pause(paused);
    }
    if (k & KEY_L)
      video_seek(s.clock_ms - 5000);
    if (k & KEY_R)
      video_seek(s.clock_ms + 5000);
    if (sys_millis() - drawn >= 500u) {
      drawn = sys_millis();
      stats_screen(&s);
    }
  }
  video_stats(&s);
  if (video_error()[0])
    snprintf(last, sizeof(last), "%s: %s", clip, video_error());
  else
    result(clip, &s);
  video_close();
}

int main(void) {
  fs_mkdir(DIR);
  scan();
  if (!video_engine_start())
    snprintf(last, sizeof(last), "No engine: %s", video_error());
  menu();
  while (app_loop()) {
    uint32_t k = hid_keys_down();
    if (k & KEY_B)
      break;
    if ((k & KEY_DOWN) && sel + 1 < n_clips)
      sel++;
    else if ((k & KEY_UP) && sel > 0)
      sel--;
    else if ((k & KEY_A) && n_clips && video_engine_running())
      play(clips[sel]);
    else if (!k)
      continue;
    menu();
  }
  video_engine_stop();
  return 0;
}
