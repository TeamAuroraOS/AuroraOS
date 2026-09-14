#ifndef AURORA_H
#define AURORA_H

#include <stdint.h>
#include <stddef.h>

#define REG_BASE_IO         0x10000000

#define REG_CFG_UNITINFO    (*(volatile uint8_t  *)0x10010010)

#define REG_HID_PAD         (*(volatile uint32_t *)0x10146000)
#define BUTTON_A            (1 << 0)
#define BUTTON_B            (1 << 1)
#define BUTTON_SELECT       (1 << 2)
#define BUTTON_START        (1 << 3)
#define BUTTON_DRIGHT       (1 << 4)
#define BUTTON_DLEFT        (1 << 5)
#define BUTTON_DUP          (1 << 6)
#define BUTTON_DDOWN        (1 << 7)
#define BUTTON_R            (1 << 8)
#define BUTTON_L            (1 << 9)
#define BUTTON_X            (1 << 10)
#define BUTTON_Y            (1 << 11)

#define AURORA_VERSION "Beta v0.1.0"

#define REG_SDMMC_BASE      0x10006000

#define REG_LCD_TOP_CFG     (*(volatile uint32_t *)0x10400400)
#define REG_LCD_BOT_CFG     (*(volatile uint32_t *)0x10400500)

/* Physical framebuffers. Normal drawing goes through VRAM_TOP_LA / VRAM_BOT_A. */
#define VRAM_TOP_PHYS       ((volatile uint8_t *)0x18300000)
#define VRAM_BOT_PHYS       ((volatile uint8_t *)0x18346500)

/* Backbuffers in cached FCRAM, clear of the Wi-Fi firmware slots (which end by
 * 0x23E70000) and the app-launch staging area at 0x24000000. */
#define VRAM_TOP_BACK       ((volatile uint8_t *)0x23E80000)
#define VRAM_BOT_BACK       ((volatile uint8_t *)0x23F00000)

/* Current draw targets: the physical framebuffers until
 * screen_use_backbuffer(1) redirects them to FCRAM. */
/* Set while an async blit may still read a backbuffer; drawing into that buffer
 * waits for it first. */
extern volatile uint8_t *g_blit_src;
extern void (*g_screen_wait)(void);
static inline void screen_touch(volatile uint8_t *fb) {
  if (g_blit_src == fb) {
    g_blit_src = 0;
    if (g_screen_wait)
      g_screen_wait();
  }
}

extern volatile uint8_t *g_fb_top;
extern volatile uint8_t *g_fb_bot;
#define VRAM_TOP_LA         (g_fb_top)
#define VRAM_BOT_A          (g_fb_bot)

#define TOP_SCREEN_WIDTH    400
#define TOP_SCREEN_HEIGHT   240
#define BOT_SCREEN_WIDTH    320
#define BOT_SCREEN_HEIGHT   240
#define BYTES_PER_PIXEL     3  

#define TOP_FB_SIZE         (TOP_SCREEN_HEIGHT * TOP_SCREEN_WIDTH * BYTES_PER_PIXEL)

#define BOT_FB_SIZE         (BOT_SCREEN_HEIGHT * BOT_SCREEN_WIDTH * BYTES_PER_PIXEL)

#define REG_SD_STATUS       (*(volatile uint16_t *)0x1000601C)
#define SD_STATUS_INSERTED  0x0000

#define FAT_SECTOR_SIZE     512
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef struct {
    u8 r, g, b;
} Color;

#define COLOR_BLACK   ((Color){0x00, 0x00, 0x00})
#define COLOR_WHITE   ((Color){0xFF, 0xFF, 0xFF})
#define COLOR_RED     ((Color){0xFF, 0x00, 0x00})
#define COLOR_GREEN   ((Color){0x00, 0xFF, 0x00})
#define COLOR_BLUE    ((Color){0x00, 0x00, 0xFF})
#define COLOR_CYAN    ((Color){0x00, 0xFF, 0xFF})
#define COLOR_MAGENTA ((Color){0xFF, 0x00, 0xFF})
#define COLOR_YELLOW  ((Color){0xFF, 0xFF, 0x00})
#define COLOR_ORANGE  ((Color){0xFF, 0xA0, 0x00})
#define COLOR_AURORA  ((Color){0x64, 0xE8, 0xC8})  
#define COLOR_DARK_GRAY   ((Color){0x50, 0x50, 0x50})
#define COLOR_DARK_RED    ((Color){0xC0, 0x20, 0x20})
#define COLOR_LIGHT_GRAY  ((Color){0xA0, 0xA0, 0xA0})
#define COLOR_BG_DARK     ((Color){0x10, 0x10, 0x20})

#define COLOR_HM_BG        ((Color){0x16, 0x16, 0x16})
#define COLOR_HM_FACET     ((Color){0x24, 0x24, 0x24})
#define COLOR_HM_BAR       ((Color){0x0C, 0x0C, 0x0C})
#define COLOR_HM_CARD      ((Color){0x08, 0x08, 0x08})
#define COLOR_HM_SLOT      ((Color){0x2A, 0x2A, 0x2A})
#define COLOR_HM_SLOT_EMPTY ((Color){0x1B, 0x1B, 0x1B})
#define COLOR_HM_TEXT2     ((Color){0x9A, 0x9A, 0x9A})

/* Card colours for the info card and dialogs (icons/diolog-box.png). */
#define COLOR_PANEL_TOP    ((Color){0x45, 0x45, 0x49})
#define COLOR_PANEL_BOT    ((Color){0x39, 0x39, 0x3D})

#define COLOR_HM_BG_TOP    ((Color){0x1F, 0x1F, 0x24})
#define COLOR_HM_BG_BOT    ((Color){0x0E, 0x0E, 0x12})
#define COLOR_HM_SLOT_TOP  ((Color){0x36, 0x36, 0x3C})
#define COLOR_HM_SLOT_BOT  ((Color){0x1E, 0x1E, 0x22})
#define COLOR_HM_EMPTY_TOP ((Color){0x1E, 0x1E, 0x22})
#define COLOR_HM_EMPTY_BOT ((Color){0x14, 0x14, 0x17})

#define REG_LCD_TOP_BRIGHTNESS  (*(volatile u32 *)0x10202240)
#define REG_LCD_BOT_BRIGHTNESS  (*(volatile u32 *)0x10202A40)

void screen_init(void);
void screen_present_top(void);
void screen_present_bottom(void);

/* Redirect drawing to the FCRAM backbuffers (1) or straight to VRAM (0).
 * With backbuffers on, nothing reaches the panel until screen_present_*(). */
void screen_use_backbuffer(int on);

/* Present hook: (src, dst, bytes) -> non-zero on success. NULL falls back to a
 * CPU copy. A hook rather than a call because screen.c is also linked into the
 * firm, which has no GPU driver. */
extern int (*g_screen_blit)(u32 src, u32 dst, u32 len);
void clear_screen(volatile u8 *fb, u32 fb_size, Color color);
void draw_pixel(volatile u8 *fb, int x, int y, int screen_height, Color color);
void draw_char(volatile u8 *fb, int x, int y, int screen_height, char c, Color fg, Color bg);
void draw_string(volatile u8 *fb, int x, int y, int screen_height, const char *str, Color fg, Color bg);
void draw_aurora_logo(volatile u8 *fb, int x0, int y0, int screen_height, Color color);

void draw_filled_rect(volatile u8 *fb, int x, int y, int w, int h, int screen_height, Color color);

/* Blend `color` into the pixel already there. alpha is 0..256. The anti-aliased
 * primitives below are built on this. */
void draw_filled_rect_alpha(volatile u8 *fb, int x, int y, int w, int h,
                            int screen_height, Color color, int alpha);
void draw_pixel_alpha(volatile u8 *fb, int x, int y, int screen_height,
                      Color color, int alpha);

void draw_vgradient(volatile u8 *fb, int x, int y, int w, int h,
                    int screen_height, Color top, Color bottom);

void draw_gradient_round_rect(volatile u8 *fb, int x, int y, int w, int h,
                              int radius, int screen_height, Color top,
                              Color bottom);

/* Text drawn at `scale` times size with smoothed edges. */
void draw_string_scaled(volatile u8 *fb, int x, int y, int screen_height,
                        const char *str, Color color, int scale);
void draw_filled_round_rect(volatile u8 *fb, int x, int y, int w, int h, int radius, int screen_height, Color color);
void draw_icon_32(volatile u8 *fb, int x, int y, int screen_height, const unsigned char *icon_bits, Color color);
void draw_icon_scaled(volatile u8 *fb, int x, int y, int screen_height, const unsigned char *icon_bits, Color color, int scale);

/* Pack art and text. Declared with opaque struct pointers so aurora.h does not
 * have to pull in assets.h; callers that draw assets include it themselves. */
struct Asset;
struct Font;
void draw_asset(volatile u8 *fb, int x, int y, int screen_height,
                const struct Asset *a, Color tint);
int draw_asset_boxed(volatile u8 *fb, int bx, int by, int bw, int bh,
                     int screen_height, uint32_t id, Color tint);
int text_width(const struct Font *f, const char *s);
int text_line_height(const struct Font *f);
int text_ascent(const struct Font *f);
void draw_text(volatile u8 *fb, int x, int baseline, int screen_height,
               const char *s, Color color, const struct Font *f);
void draw_text_centered(volatile u8 *fb, int cx, int baseline,
                        int screen_height, const char *s, Color color,
                        const struct Font *f);

void console_init(void);
void console_print(const char *str);
void console_print_color(const char *str, Color color);

void ui_draw_home_screen(void);
int  ui_power_off_confirm(void);
void ui_settings_menu(void);

int  sd_init(void);
int  sd_is_inserted(void);
int  check_files_directory(void);

void delay(volatile u32 cycles);
void power_off(void);
u32  get_keys(void);
u32  get_keys_down(void);
void set_brightness(u32 level);

#endif
