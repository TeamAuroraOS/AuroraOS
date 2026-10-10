// Demo: a tour of the AuroraOS C SDK.
//
// Top screen: shapes, the four UI fonts, a ball the circle pad pushes around,
// the clock, the battery and the frame rate. Bottom screen: paint with the
// stylus, pick colours from the bar, X saves the picture as a BMP in the
// app's data folder with the C library's stdio.
//
// Optional files in the app's data folder (SD:/Aurora/Apps/C/Demo/):
//   sprite.png  drawn on the ball, with its alpha
//   bounce.wav  played when the ball hits an edge
//
// A: new ball colour   X: save the picture   Y: clear it   START: quit
#include <aurora_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_H 200
#define BAR_Y CANVAS_H

static const uint32_t palette[] = {
    COLOR_BLACK, 0xFF3B30, 0xFF9F0A, 0xFFD60A, 0x34C83A,
    0x32D6E2,    0x3B82F6, 0xA05CE2, 0xFF2DB8, COLOR_WHITE,
};
#define COLORS (int)(sizeof(palette) / sizeof(palette[0]))

static uint32_t paper = RGB(250, 248, 240);
static int brush = 0;
static char status[320] = "Draw with the stylus";

static void draw_bar(void) {
  int w = SCREEN_BOTTOM_WIDTH / COLORS;
  gfx_rect(SCREEN_BOTTOM, 0, BAR_Y, SCREEN_BOTTOM_WIDTH, 40, RGB(30, 30, 36));
  for (int i = 0; i < COLORS; i++) {
    gfx_round_rect(SCREEN_BOTTOM, i * w + 4, BAR_Y + 8, w - 8, 24, 6,
                   palette[i]);
    if (i == brush)
      gfx_rect_outline(SCREEN_BOTTOM, i * w + 1, BAR_Y + 5, w - 2, 30, 2,
                       COLOR_AURORA);
  }
}

static void clear_canvas(void) {
  gfx_rect(SCREEN_BOTTOM, 0, 0, SCREEN_BOTTOM_WIDTH, CANVAS_H, paper);
}

// A thick stroke: discs along the line.
static void stroke(int x0, int y0, int x1, int y1, uint32_t color) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  for (int i = 0; i <= steps; i++) {
    int x = steps ? x0 + dx * i / steps : x0;
    int y = steps ? y0 + dy * i / steps : y0;
    if (y < CANVAS_H - 3)
      gfx_circle(SCREEN_BOTTOM, x, y, 3, color);
  }
}

static void put16(FILE *f, unsigned v) {
  fputc(v & 0xFF, f);
  fputc((v >> 8) & 0xFF, f);
}

static void put32(FILE *f, unsigned v) {
  put16(f, v & 0xFFFF);
  put16(f, v >> 16);
}

// The canvas as a 24-bit BMP, rows bottom first, written with stdio.
static bool save_bmp(const char *path) {
  int w = SCREEN_BOTTOM_WIDTH, h = CANVAS_H, row = w * 3;
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  fputs("BM", f);
  put32(f, 54 + row * h);
  put32(f, 0);
  put32(f, 54);
  put32(f, 40);
  put32(f, w);
  put32(f, h);
  put16(f, 1);
  put16(f, 24);
  put32(f, 0);
  put32(f, row * h);
  put32(f, 2835);
  put32(f, 2835);
  put32(f, 0);
  put32(f, 0);
  for (int y = h - 1; y >= 0; y--)
    for (int x = 0; x < w; x++) {
      uint32_t c = gfx_get_pixel(SCREEN_BOTTOM, x, y);
      fputc(c & 0xFF, f);
      fputc((c >> 8) & 0xFF, f);
      fputc((c >> 16) & 0xFF, f);
    }
  return fclose(f) == 0;
}

static void save(void) {
  char path[300];
  snprintf(path, sizeof(path), "%s/drawing.bmp", app_dir());
  if (fs_mkdir(app_dir()) && save_bmp(path))
    snprintf(status, sizeof(status), "Saved %s", path);
  else
    snprintf(status, sizeof(status), "Could not save to the SD card");
}

static void draw_top(float bx, float by, uint32_t ball, GfxImage *sprite,
                     int frame) {
  SysTime t;
  char clock[16] = "--:--";
  int battery = sys_battery();

  gfx_gradient(SCREEN_TOP, 0, 0, SCREEN_TOP_WIDTH, SCREEN_HEIGHT,
               RGB(20, 24, 44), RGB(8, 8, 14));

  gfx_print(SCREEN_TOP, 14, 10, FONT_TITLE, COLOR_WHITE, "AuroraOS C SDK");
  gfx_print(SCREEN_TOP, 14, 36, FONT_REGULAR, COLOR_GRAY,
            "Hello, %s!", sys_user_name()[0] ? sys_user_name() : "world");
  if (sys_time(&t))
    snprintf(clock, sizeof(clock), "%02d:%02d", t.hour, t.minute);
  gfx_print(SCREEN_TOP, 300, 12, FONT_BOLD, COLOR_WHITE, "%s", clock);
  gfx_print(SCREEN_TOP, 300, 30, FONT_SMALL, COLOR_GRAY, "%d fps", app_fps());
  if (battery >= 0)
    gfx_print(SCREEN_TOP, 350, 30, FONT_SMALL, COLOR_GRAY, "%d%%", battery);

  // Shapes, along the bottom.
  gfx_triangle(SCREEN_TOP, 20, 220, 50, 170, 80, 220, 0xFF9F0A);
  gfx_round_rect(SCREEN_TOP, 96, 172, 64, 48, 12, 0x3B82F6);
  gfx_circle_outline(SCREEN_TOP, 200, 196, 24, COLOR_AURORA);
  for (int i = 0; i < 6; i++)
    gfx_line(SCREEN_TOP, 240 + i * 8, 220, 260 + i * 12, 172, 0xFF2DB8);
  gfx_rect_blend(SCREEN_TOP, 320, 172, 60, 48, COLOR_WHITE, 96);
  gfx_text(SCREEN_TOP, 326, 190, COLOR_BLACK, "blend");

  gfx_text_scaled(SCREEN_TOP, 14, 64, 2, 0xFFD60A, "8x8 x2");
  gfx_text(SCREEN_TOP, 14, 86, COLOR_WHITE, "Frame %d", frame);
  if (!gfx_fonts_loaded())
    gfx_text(SCREEN_TOP, 14, 100, 0xFF3B30, "(no assets.pak: 8x8 font only)");

  if (sprite)
    gfx_image(SCREEN_TOP, sprite, (int)bx - sprite->w / 2,
              (int)by - sprite->h / 2);
  else
    gfx_circle(SCREEN_TOP, (int)bx, (int)by, 14, ball);
}

int main(void) {
  float bx = 200, by = 120, vx = 2.2f, vy = 1.6f;
  uint32_t ball = COLOR_AURORA;
  int last_x = -1, last_y = -1, frame = 0, hits = 0;
  char path[300];
  GfxImage *sprite;
  int bounce;
  float r; // the ball's radius

  snprintf(path, sizeof(path), "%s/sprite.png", app_dir());
  sprite = gfx_image_load(path);
  snprintf(path, sizeof(path), "%s/bounce.wav", app_dir());
  bounce = snd_load(path);
  r = sprite ? sprite->w / 2 : 14;

  clear_canvas();
  draw_bar();

  while (app_loop()) {
    uint32_t down = hid_keys_down();
    int cx, cy, w = SCREEN_BOTTOM_WIDTH / COLORS;

    if (down & KEY_START)
      break;
    if (down & KEY_A)
      ball = palette[1 + rand() % (COLORS - 2)];
    if (down & KEY_X)
      save();
    if (down & KEY_Y) {
      clear_canvas();
      strcpy(status, "Cleared");
    }

    // The ball: the circle pad pushes, the edges bounce.
    hid_cpad(&cx, &cy);
    vx += cx / 400.0f;
    vy -= cy / 400.0f;
    vx *= 0.995f;
    vy *= 0.995f;
    bx += vx;
    by += vy;
    if (bx < r || bx > SCREEN_TOP_WIDTH - r) {
      vx = -vx;
      bx = bx < r ? r : SCREEN_TOP_WIDTH - r;
      hits++;
    }
    if (by < r || by > SCREEN_HEIGHT - r) {
      vy = -vy;
      by = by < r ? r : SCREEN_HEIGHT - r;
      hits++;
    }
    if (hits) {
      snd_play(bounce);
      hits = 0;
    }

    // Painting, and the colour bar.
    if (hid_touch_held()) {
      int x = hid_touch_x(), y = hid_touch_y();
      if (y >= BAR_Y) {
        if (hid_touch_down()) {
          brush = x / w < COLORS ? x / w : COLORS - 1;
          draw_bar();
        }
      } else {
        stroke(last_x < 0 ? x : last_x, last_x < 0 ? y : last_y, x, y,
               palette[brush]);
        last_x = x;
        last_y = y;
      }
    } else {
      last_x = -1;
    }

    draw_top(bx, by, ball, sprite, frame++);
    gfx_rect(SCREEN_TOP, 0, SCREEN_HEIGHT - 14, SCREEN_TOP_WIDTH, 14,
             RGB(8, 8, 14));
    gfx_print(SCREEN_TOP, 6, SCREEN_HEIGHT - 13, FONT_SMALL, COLOR_GRAY,
              "%s", status);
  }

  gfx_image_free(sprite);
  return 0;
}
