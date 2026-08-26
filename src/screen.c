

#include "aurora.h"
#include "aurora_logo.h"
#include "font.h"
#include "icons.h"

static int console_x = 0;
static int console_y = 0;

#define CONSOLE_COLS (BOT_SCREEN_WIDTH / FONT_WIDTH)   
#define CONSOLE_ROWS (BOT_SCREEN_HEIGHT / FONT_HEIGHT) 

static Color console_fg = {0xFF, 0xFF, 0xFF};
static Color console_bg = {0x10, 0x10, 0x20};

void screen_init(void) {
  Color top_bg = {0x05, 0x0A, 0x15};
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, top_bg);

  
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);
  
  
  screen_present_top();
  screen_present_bottom();
}

void screen_present_top(void) {}

void screen_present_bottom(void) {}

void clear_screen(volatile u8 *fb, u32 fb_size, Color color) {
  /* Fill the framebuffer with a repeating 32-bit pattern. */
  u32 b = color.b, g = color.g, r = color.r;
  u32 w0 = b | (g << 8) | (r << 16) | (b << 24);
  u32 w1 = g | (r << 8) | (b << 16) | (g << 24);
  u32 w2 = r | (b << 8) | (g << 16) | (r << 24);
  volatile u32 *p = (volatile u32 *)fb;
  volatile u32 *end = p + (fb_size >> 2);
  while (p < end) {
    p[0] = w0;
    p[1] = w1;
    p[2] = w2;
    p += 3;
  }
}

void draw_pixel(volatile u8 *fb, int x, int y, int screen_height, Color color) {
  if (x < 0 || y < 0 || y >= screen_height)
    return;

  
  u32 offset =
      ((x * screen_height) + (screen_height - 1 - y)) * BYTES_PER_PIXEL;
  fb[offset + 0] = color.b;
  fb[offset + 1] = color.g;
  fb[offset + 2] = color.r;
}

void draw_char(volatile u8 *fb, int x, int y, int screen_height, char c,
               Color fg, Color bg) {
  
  if (c < 0x20 || c > 0x7E)
    return;

  const u8 *glyph = font_data[c - 0x20];

  for (int row = 0; row < FONT_HEIGHT; row++) {
    u8 bits = glyph[row];
    for (int col = 0; col < FONT_WIDTH; col++) {
      Color pixel = (bits & (0x80 >> col)) ? fg : bg;
      draw_pixel(fb, x + col, y + row, screen_height, pixel);
    }
  }
}

void draw_string(volatile u8 *fb, int x, int y, int screen_height,
                 const char *str, Color fg, Color bg) {
  int cx = x;
  while (*str) {
    if (*str == '\n') {
      cx = x;
      y += FONT_HEIGHT;
    } else {
      draw_char(fb, cx, y, screen_height, *str, fg, bg);
      cx += FONT_WIDTH;
    }
    str++;
  }
}

void draw_aurora_logo(volatile u8 *fb, int x0, int y0, int screen_height,
                      Color color) {
  for (int y = 0; y < AURORA_LOGO_HEIGHT; y++) {
    for (int x = 0; x < AURORA_LOGO_WIDTH; x++) {
      int byte_idx = y * AURORA_LOGO_ROW_BYTES + (x / 8);
      int bit_idx = 7 - (x % 8);
      if (aurora_logo_bits[byte_idx] & (1 << bit_idx)) {
        draw_pixel(fb, x0 + x, y0 + y, screen_height, color);
      }
    }
  }
}

void draw_filled_rect(volatile u8 *fb, int x, int y, int w, int h,
                      int screen_height, Color color) {
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > screen_height)
    h = screen_height - y;
  if (w <= 0 || h <= 0)
    return;

  u8 b = color.b, g = color.g, r = color.r;
  /* Store one offset per column to avoid multiplying inside the inner loop. */
  for (int px = x; px < x + w; px++) {
    volatile u8 *p = fb + ((px * screen_height) + (screen_height - 1 - y)) * 3;
    for (int i = 0; i < h; i++) {
      p[0] = b;
      p[1] = g;
      p[2] = r;
      p -= 3;
    }
  }
}

/* Fill one rounded corner using a simple circle test. */
static void fill_corner(volatile u8 *fb, int bx, int by, int r, int cx0,
                        int cy0, int screen_height, Color color) {
  int r2 = r * r;
  for (int cy = by; cy < by + r; cy++) {
    for (int cx = bx; cx < bx + r; cx++) {
      int ex = cx - cx0, ey = cy - cy0;
      if (ex * ex + ey * ey <= r2)
        draw_pixel(fb, cx, cy, screen_height, color);
    }
  }
}

void draw_filled_round_rect(volatile u8 *fb, int x, int y, int w, int h,
                            int radius, int screen_height, Color color) {
  if (radius < 0)
    radius = 0;
  if (radius > w / 2)
    radius = w / 2;
  if (radius > h / 2)
    radius = h / 2;

  draw_filled_rect(fb, x + radius, y, w - 2 * radius, h, screen_height, color);
  draw_filled_rect(fb, x, y + radius, radius, h - 2 * radius, screen_height,
                   color);
  draw_filled_rect(fb, x + w - radius, y + radius, radius, h - 2 * radius,
                   screen_height, color);

  if (radius > 0) {
    fill_corner(fb, x, y, radius, x + radius, y + radius, screen_height, color);
    fill_corner(fb, x + w - radius, y, radius, x + w - 1 - radius, y + radius,
                screen_height, color);
    fill_corner(fb, x, y + h - radius, radius, x + radius, y + h - 1 - radius,
                screen_height, color);
    fill_corner(fb, x + w - radius, y + h - radius, radius, x + w - 1 - radius,
                y + h - 1 - radius, screen_height, color);
  }
}

void draw_icon_32(volatile u8 *fb, int x, int y, int screen_height,
                  const unsigned char *icon_bits, Color color) {
  for (int row = 0; row < ICON_SIZE; row++) {
    for (int col = 0; col < ICON_SIZE; col++) {
      int byte_idx = row * ICON_ROW_BYTES + (col / 8);
      int bit_idx = 7 - (col % 8);
      if (icon_bits[byte_idx] & (1 << bit_idx)) {
        draw_pixel(fb, x + col, y + row, screen_height, color);
      }
    }
  }
}

void draw_icon_scaled(volatile u8 *fb, int x, int y, int screen_height,
                      const unsigned char *icon_bits, Color color, int scale) {
  for (int row = 0; row < ICON_SIZE; row++) {
    for (int col = 0; col < ICON_SIZE; col++) {
      int byte_idx = row * ICON_ROW_BYTES + (col / 8);
      if (icon_bits[byte_idx] & (0x80 >> (col % 8))) {
        draw_filled_rect(fb, x + col * scale, y + row * scale, scale, scale,
                         screen_height, color);
      }
    }
  }
}

static void console_scroll(void) {
  volatile u8 *fb = VRAM_BOT_A;

  
  for (int x = 0; x < BOT_SCREEN_WIDTH; x++) {
    for (int y = 0; y < BOT_SCREEN_HEIGHT - FONT_HEIGHT; y++) {
      u32 dst_off = ((x * BOT_SCREEN_HEIGHT) + (BOT_SCREEN_HEIGHT - 1 - y)) * 3;
      u32 src_off = ((x * BOT_SCREEN_HEIGHT) +
                     (BOT_SCREEN_HEIGHT - 1 - (y + FONT_HEIGHT))) *
                    3;
      fb[dst_off + 0] = fb[src_off + 0];
      fb[dst_off + 1] = fb[src_off + 1];
      fb[dst_off + 2] = fb[src_off + 2];
    }
    
    for (int y = BOT_SCREEN_HEIGHT - FONT_HEIGHT; y < BOT_SCREEN_HEIGHT; y++) {
      u32 off = ((x * BOT_SCREEN_HEIGHT) + (BOT_SCREEN_HEIGHT - 1 - y)) * 3;
      fb[off + 0] = console_bg.b;
      fb[off + 1] = console_bg.g;
      fb[off + 2] = console_bg.r;
    }
  }
}

void console_init(void) {
  console_x = 0;
  console_y = 0;
  console_fg = COLOR_WHITE;
  console_bg = (Color){0x10, 0x10, 0x20};

  
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, console_bg);
}

void console_print(const char *str) { console_print_color(str, console_fg); }

void console_print_color(const char *str, Color color) {
  while (*str) {
    if (*str == '\n') {
      console_x = 0;
      console_y++;
    } else {
      
      int px = console_x * FONT_WIDTH;
      int py = console_y * FONT_HEIGHT;
      draw_char(VRAM_BOT_A, px, py, BOT_SCREEN_HEIGHT, *str, color, console_bg);
      console_x++;

      
      if (console_x >= CONSOLE_COLS) {
        console_x = 0;
        console_y++;
      }
    }

    
    if (console_y >= CONSOLE_ROWS) {
      console_scroll();
      console_y = CONSOLE_ROWS - 1;
      console_x = 0;
    }

    str++;
  }
}
