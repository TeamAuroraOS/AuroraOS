// Bricks: a small breakout game in C for AuroraOS.
//
// Left/Right, the circle pad or a finger on the bottom screen move the paddle;
// A launches the ball; START quits. The best score is kept in the app's data
// folder (highscore.txt) and saved through atexit(), so pressing HOME keeps
// it too. Optional sounds there: paddle.wav, brick.wav, lost.wav.
#include <aurora_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define COLS 10
#define ROWS 6
#define BRICK_W 36
#define BRICK_H 12
#define GAP 4
#define TOP 30
#define PADDLE_Y 222
#define PADDLE_H 6
#define BALL_R 4
#define FIX 256 // positions in 1/256 pixel

static const uint32_t row_color[ROWS] = {0xFF3B30, 0xFF9F0A, 0xFFD60A,
                                         0x34C83A, 0x32D6E2, 0x3B82F6};

static bool brick[ROWS][COLS];
static int bricks_left, score, best, lives = 3, level = 1;
static int paddle_x = 200, paddle_w = 64;
static int ball_x, ball_y, ball_vx, ball_vy; // fixed point
static bool ball_free;
static int snd_paddle = -1, snd_brick = -1, snd_lost = -1;
static char best_path[300];

static void load_best(void) {
  FILE *f;
  snprintf(best_path, sizeof(best_path), "%s/highscore.txt", app_dir());
  f = fopen(best_path, "r");
  if (f) {
    if (fscanf(f, "%d", &best) != 1)
      best = 0;
    fclose(f);
  }
}

static void save_best(void) {
  FILE *f;
  if (score > best)
    best = score;
  if (!best || !fs_mkdir(app_dir()))
    return;
  f = fopen(best_path, "w");
  if (f) {
    fprintf(f, "%d\n", best);
    fclose(f);
  }
}

static void sound(const char *name, int *handle) {
  char path[300];
  snprintf(path, sizeof(path), "%s/%s", app_dir(), name);
  *handle = snd_load(path);
}

static void new_level(void) {
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      brick[r][c] = true;
  bricks_left = ROWS * COLS;
}

static void serve(void) {
  ball_free = false;
  ball_vx = (rand() % 2 ? 1 : -1) * (180 + level * 20);
  ball_vy = -(300 + level * 30);
}

// The wall is centred: COLS bricks and the gaps between them.
static int brick_x(int c) {
  return (SCREEN_TOP_WIDTH - COLS * BRICK_W - (COLS - 1) * GAP) / 2 +
         c * (BRICK_W + GAP);
}
static int brick_y(int r) { return TOP + r * (BRICK_H + GAP); }

// The ball against one brick: true on a hit, with the bounce applied.
static bool hit_brick(int r, int c) {
  int bx = ball_x / FIX, by = ball_y / FIX;
  int x0 = brick_x(c), y0 = brick_y(r), x1 = x0 + BRICK_W, y1 = y0 + BRICK_H;
  int over_x, over_y;

  if (bx + BALL_R < x0 || bx - BALL_R > x1 || by + BALL_R < y0 ||
      by - BALL_R > y1)
    return false;
  // Bounce off the side with the smaller overlap.
  over_x = bx < (x0 + x1) / 2 ? bx + BALL_R - x0 : x1 - (bx - BALL_R);
  over_y = by < (y0 + y1) / 2 ? by + BALL_R - y0 : y1 - (by - BALL_R);
  if (over_x < over_y)
    ball_vx = -ball_vx;
  else
    ball_vy = -ball_vy;
  return true;
}

static void step(void) {
  int bx, by;

  if (!ball_free) {
    ball_x = paddle_x * FIX;
    ball_y = (PADDLE_Y - BALL_R - 1) * FIX;
    return;
  }
  ball_x += ball_vx;
  ball_y += ball_vy;
  bx = ball_x / FIX;
  by = ball_y / FIX;

  if (bx < BALL_R || bx > SCREEN_TOP_WIDTH - BALL_R) {
    ball_vx = -ball_vx;
    ball_x = (bx < BALL_R ? BALL_R : SCREEN_TOP_WIDTH - BALL_R) * FIX;
  }
  if (by < TOP - 10 + BALL_R) {
    ball_vy = -ball_vy;
    ball_y = (TOP - 10 + BALL_R) * FIX;
  }

  // The paddle sends the ball off at an angle from where it lands.
  if (ball_vy > 0 && by + BALL_R >= PADDLE_Y && by < PADDLE_Y + PADDLE_H &&
      abs(bx - paddle_x) <= paddle_w / 2 + BALL_R) {
    int off = (bx - paddle_x) * 100 / (paddle_w / 2);
    ball_vx = off * (4 + level) / 2;
    ball_vy = -abs(ball_vy);
    snd_play(snd_paddle);
  }

  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (brick[r][c] && hit_brick(r, c)) {
        brick[r][c] = false;
        bricks_left--;
        score += 10 * (ROWS - r);
        snd_play(snd_brick);
        r = ROWS; // one brick per step
        break;
      }

  if (by > SCREEN_HEIGHT + BALL_R) {
    snd_play(snd_lost);
    if (--lives <= 0) {
      save_best();
      score = 0;
      lives = 3;
      level = 1;
      new_level();
    }
    serve();
  }
  if (!bricks_left) {
    level++;
    new_level();
    serve();
  }
}

static void draw(void) {
  gfx_gradient(SCREEN_TOP, 0, 0, SCREEN_TOP_WIDTH, SCREEN_HEIGHT,
               RGB(18, 20, 36), RGB(6, 6, 10));
  gfx_print(SCREEN_TOP, 6, 4, FONT_BOLD, COLOR_WHITE, "Score %d", score);
  gfx_print(SCREEN_TOP, 300, 4, FONT_BOLD, COLOR_GRAY, "Best %d",
            score > best ? score : best);
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (brick[r][c])
        gfx_round_rect(SCREEN_TOP, brick_x(c), brick_y(r), BRICK_W, BRICK_H,
                       3, row_color[r]);
  gfx_round_rect(SCREEN_TOP, paddle_x - paddle_w / 2, PADDLE_Y, paddle_w,
                 PADDLE_H, 3, COLOR_WHITE);
  gfx_circle(SCREEN_TOP, ball_x / FIX, ball_y / FIX, BALL_R, COLOR_AURORA);

  gfx_clear(SCREEN_BOTTOM, RGB(24, 24, 30));
  gfx_print(SCREEN_BOTTOM, 16, 14, FONT_TITLE, COLOR_WHITE, "Bricks");
  gfx_print(SCREEN_BOTTOM, 16, 46, FONT_REGULAR, COLOR_GRAY,
            "Level %d     Lives %d", level, lives);
  gfx_print(SCREEN_BOTTOM, 16, 80, FONT_SMALL, COLOR_GRAY,
            "Left/Right, the circle pad or drag below: move\n"
            "A: launch the ball      START: quit");
  gfx_round_rect(SCREEN_BOTTOM, 16, 150, 288, 60, 10, RGB(40, 40, 48));
  gfx_round_rect(SCREEN_BOTTOM, 16 + (paddle_x - paddle_w / 2) * 288 / 400,
                 172, paddle_w * 288 / 400, 16, 6, COLOR_AURORA);
  if (!ball_free) {
    int w = gfx_text_width(FONT_BOLD, "Press A");
    gfx_print(SCREEN_TOP, (SCREEN_TOP_WIDTH - w) / 2, 160, FONT_BOLD,
              COLOR_WHITE, "Press A");
  }
}

int main(void) {
  srand((unsigned)time(NULL));
  load_best();
  atexit(save_best);
  sound("paddle.wav", &snd_paddle);
  sound("brick.wav", &snd_brick);
  sound("lost.wav", &snd_lost);
  new_level();
  serve();

  while (app_loop()) {
    uint32_t held = hid_keys_held();
    int cx;

    if (hid_keys_down() & KEY_START)
      break;
    if (hid_keys_down() & KEY_A)
      ball_free = true;
    hid_cpad(&cx, NULL);
    if (held & KEY_LEFT)
      paddle_x -= 5;
    if (held & KEY_RIGHT)
      paddle_x += 5;
    paddle_x += cx / 20;
    if (hid_touch_held() && hid_touch_y() >= 140)
      paddle_x = (hid_touch_x() - 16) * 400 / 288;
    if (paddle_x < paddle_w / 2)
      paddle_x = paddle_w / 2;
    if (paddle_x > SCREEN_TOP_WIDTH - paddle_w / 2)
      paddle_x = SCREEN_TOP_WIDTH - paddle_w / 2;

    // Two steps a frame: in each the ball moves less than a brick is high,
    // so it cannot pass through one.
    step();
    step();
    draw();
  }
  return 0;
}
