/* Time, the clock, the battery and the user's settings. */
#include "app_internal.h"
#include "crash.h"
#include "model.h"
#include "power.h"
#include "timer.h"
#include "user.h"

#include <stdio.h>
#include <string.h>

/* The hardware count is 32 bits at 65,457 Hz and wraps after about 18 hours;
 * every caller passes through here often enough to see each wrap. */
u64 app_ticks64(void) {
  static u32 last, high;
  u32 t = timer_ticks();
  if (t < last)
    high++;
  last = t;
  return ((u64)high << 32) | t;
}

uint32_t sys_millis(void) {
  return (uint32_t)(app_ticks64() * 1000u / timer_hz());
}

uint64_t sys_micros(void) { return app_ticks64() * 1000000u / timer_hz(); }

/* HOME and an ARM11 fault are checked once a frame meanwhile. */
void app_wait_ticks(u64 ticks) {
  u64 now = app_ticks64(), end = now + ticks, next = now;
  u32 frame = timer_hz() / 60u;
  while (now < end) {
    if (now >= next) {
      app_poll_home();
      crash_poll_arm11();
      next = now + frame;
    }
    now = app_ticks64();
  }
}

void sys_sleep(uint32_t ms) { app_wait_ticks((u64)ms * timer_hz() / 1000u); }

/* Days from 1 March of year 0 (Howard Hinnant's days_from_civil), then
 * seconds from 1970. */
long long app_civil_secs(int y, int m, int d, int hh, int mi, int ss) {
  long long era, days;
  unsigned yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  days = era * 146097 + (long long)doe - 719468;
  return days * 86400 + hh * 3600 + mi * 60 + ss;
}

static s16 get16(const u8 *p) { return (s16)(p[0] | (p[1] << 8)); }

bool sys_time(SysTime *t) {
  static bool offset_read;
  RtcTime r;

  if (!offset_read) {
    const u8 *d = app_user_dat();
    s16 off = d ? get16(d + USER_DAT_CLOCK) : 0;
    g_rtc_offset = off > -1440 && off < 1440 ? off : 0;
    offset_read = true;
  }
  if (!t || !rtc_read(&r))
    return false;
  t->year = r.year;
  t->month = r.month;
  t->day = r.day;
  t->hour = r.hour;
  t->minute = r.min;
  t->second = r.sec;
  t->weekday = r.wday;
  return true;
}

int sys_battery(void) { return battery_percent(); }
int sys_battery_tenths(void) { return battery_tenths(); }
int sys_battery_temperature(void) { return battery_temperature(); }
int sys_battery_millivolts(void) { return battery_millivolts(); }
bool sys_charging(void) { return battery_charging() == 1; }
bool sys_is_new3ds(void) { return app_core() && aurora_is_new3ds(); }

const char *sys_user_name(void) {
  static char name[USER_NAME_MAX];
  const u8 *d = app_user_dat();
  if (!d)
    return "";
  memcpy(name, d + 12, USER_NAME_MAX - 1);
  name[USER_NAME_MAX - 1] = '\0';
  return name;
}

SysLanguage sys_language(void) {
  const u8 *d = app_user_dat();
  return d && d[6] < LANG_COUNT ? (SysLanguage)d[6] : SYS_LANG_ENGLISH;
}

/* aurora_accent_presets in src/os/os_setup.c, which apps do not link. */
static const uint32_t accents[AURORA_ACCENT_COUNT] = {
    0x64E8C8, 0xFF3B30, 0xFF9F0A, 0xFFD60A, 0x34C83A, 0x1E8A3B, 0x32D6E2,
    0x3B82F6, 0x282FE6, 0xA05CE2, 0xFF2DB8, 0xFF2D55, 0xB0B8E8, 0x9A9A9A,
};

uint32_t sys_accent(void) {
  const u8 *d = app_user_dat();
  return accents[d && d[7] < AURORA_ACCENT_COUNT ? d[7] : 0];
}

/* Weak, like app_flush_stdout(): only an app that uses stdio has files in
 * its buffers. */
extern int fflush(FILE *f) __attribute__((weak));

static void before_power(void) {
  if (fflush)
    fflush(NULL);
  app_fs_close_all();
  app_snd_leave();
}

void sys_power_off(void) {
  before_power();
  power_shutdown();
  for (;;)
    ;
}

void sys_reboot(void) {
  before_power();
  power_reboot();
  for (;;)
    ;
}
