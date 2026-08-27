/* Auric runtime shim -- implementation.
 *
 * Bridges the four Auric built-ins onto AuroraOS's existing API. The drawing
 * primitives (draw_string, clear_screen, ...) come from AuroraOS's own
 * src/screen.c, which aurc compiles alongside this file; the two tiny input/
 * timing helpers (get_keys_down, delay) live here so an Auric app does not have
 * to pull in the whole Home Menu (os_main.c).
 *
 * Everything targets the TOP screen (400x240): print and clear both draw there,
 * origin top-left, 8x8 font. This keeps the coordinate model a single, simple
 * space for v0.1 programs.
 */
#include "aurora.h"
#include "i2c.h"             /* MCU access for the HOME button        */
#include "loader.h"          /* shared HOME-return contract addresses */
#include "auric_runtime.h"

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

/* ---- input / timing (mirrors os_main.c so apps stay self-contained) ----- */
void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop");
}

u32 get_keys(void) { return ~REG_HID_PAD & 0x3FF; }

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

/* Called from every built-in so any app returns to the Home Menu on HOME. The
 * descriptor magic gates this: when an app is booted directly (not launched by
 * the Home Menu) there is no OS to return to, so HOME is ignored. */
void aur_check_home(void) {
  volatile u32 *desc = (volatile u32 *)AURORA_RETURN_DESC_ADDR;
  if (desc[0] != AURORA_RETURN_READY_MAGIC)
    return;
  if (aur_home_pressed())
    ((void (*)(void))AURORA_RETURN_STUB_ADDR)(); /* restores the OS; no return */
}

/* ---- built-ins ---------------------------------------------------------- */
void aur_print(const char *text, int x, int y, int color) {
  aur_check_home();
  draw_string(VRAM_TOP_LA, x, y, TOP_SCREEN_HEIGHT, text, aur_unpack(color),
              aur_bg);
  screen_present_top();
}

void aur_clear(int color) {
  aur_check_home();
  aur_bg = aur_unpack(color);
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, aur_bg);
  screen_present_top();
}

void aur_fill_rect(int x, int y, int w, int h, int color) {
  aur_check_home();
  draw_filled_rect(VRAM_TOP_LA, x, y, w, h, TOP_SCREEN_HEIGHT, aur_unpack(color));
  screen_present_top();
}

void aur_wait_key(int button) {
  /* Prime the edge detector so a button already held on entry is ignored. */
  get_keys_down();
  while (1) {
    aur_check_home();
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
