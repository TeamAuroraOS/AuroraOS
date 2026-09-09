/*
 * MCU real-time clock and battery gauge, read over I2C.
 *
 * The management MCU (I2C device 3, address 0x4A) exposes the console's RTC at
 * registers 0x30..0x36 as packed BCD, and the battery state at 0x0B / 0x0F.
 * The crash handler already depends on 0x30 counting seconds for its power-off
 * countdown, which is what confirms the RTC layout on real hardware.
 */
#include "power.h"
#include "i2c.h"

#define MCU_REG_BATTERY 0x0Bu /* charge percentage, 0-100                    */
#define MCU_REG_STATUS  0x0Fu /* power / charger flags                       */
#define MCU_REG_RTC     0x30u /* 7 BCD bytes: sec,min,hour,wday,day,mon,year */

/* Charger-present bit in MCU register 0x0F. See docs/power.md: the register is
 * known to carry the power flags, but which bit means "charging" is not firmly
 * documented, so it is isolated here as the one thing to change if the
 * indicator does not follow the charger. */
#define MCU_STATUS_CHARGING (1u << 4)

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

int rtc_read(RtcTime *out) {
  uint8_t r[7];

  if (!I2C_readRegBuf(I2C_DEV_MCU, MCU_REG_RTC, r, sizeof(r)))
    return 0;

  /* Mask off the flag bits some RTC fields carry above their BCD digits (the
   * hour register can hold a 12/24-hour selector) before decoding. */
  out->sec = bcd(r[0] & 0x7Fu);
  out->min = bcd(r[1] & 0x7Fu);
  out->hour = bcd(r[2] & 0x3Fu);
  out->wday = r[3] & 0x07u;
  out->day = bcd(r[4] & 0x3Fu);
  out->month = bcd(r[5] & 0x1Fu);
  out->year = 2000 + bcd(r[6]);

  /* A dead or half-initialised MCU tends to return 0x00 or 0xFF everywhere.
   * Reject anything that cannot be a real date so the UI shows "--:--" rather
   * than a convincing but wrong clock. */
  if (out->month < 1 || out->month > 12 || out->day < 1 || out->day > 31 ||
      out->hour > 23 || out->min > 59 || out->sec > 59)
    return 0;

  return 1;
}

static void two_digits(char *o, int v) {
  if (v < 0)
    v = 0;
  o[0] = (char)('0' + (v / 10) % 10);
  o[1] = (char)('0' + v % 10);
}

void rtc_format_time(const RtcTime *t, char *out) {
  two_digits(out, t->hour);
  out[2] = ':';
  two_digits(out + 3, t->min);
  out[5] = '\0';
}

static const char *const wday_names[7] = {"Sun", "Mon", "Tue", "Wed",
                                          "Thu", "Fri", "Sat"};
static const char *const month_names[12] = {"Jan", "Feb", "Mar", "Apr",
                                            "May", "Jun", "Jul", "Aug",
                                            "Sep", "Oct", "Nov", "Dec"};

void rtc_format_date(const RtcTime *t, char *out) {
  int wd = t->wday;
  int mo = t->month - 1;
  if (wd < 0 || wd > 6)
    wd = 0;
  if (mo < 0 || mo > 11)
    mo = 0;

  const char *w = wday_names[wd];
  const char *m = month_names[mo];

  out[0] = w[0];
  out[1] = w[1];
  out[2] = w[2];
  out[3] = ' ';
  two_digits(out + 4, t->day);
  out[6] = ' ';
  out[7] = m[0];
  out[8] = m[1];
  out[9] = m[2];
  out[10] = '\0';
}

int battery_percent(void) {
  uint8_t v;

  if (!I2C_readRegBuf(I2C_DEV_MCU, MCU_REG_BATTERY, &v, 1))
    return -1;
  return v > 100 ? 100 : (int)v;
}

int power_status_raw(void) {
  uint8_t v;

  if (!I2C_readRegBuf(I2C_DEV_MCU, MCU_REG_STATUS, &v, 1))
    return -1;
  return (int)v;
}

int battery_charging(void) {
  int s = power_status_raw();

  if (s < 0)
    return -1;
  return (s & MCU_STATUS_CHARGING) ? 1 : 0;
}
