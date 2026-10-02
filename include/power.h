#ifndef AURORA_POWER_H
#define AURORA_POWER_H

#include <stdint.h>

/* Call I2C_init() first. */

typedef struct {
  int year;  /* full year */
  int month; /* 1-12  */
  int day;
  int hour;
  int min;
  int sec;
  int wday;  /* 0 = Sunday */
} RtcTime;

/* Read the wall clock, moved by g_rtc_offset. Returns 1 on success, 0 if the
 * MCU did not answer or returned a value that cannot be a real date. */
int rtc_read(RtcTime *out);

/* Minutes added to the RTC before it is shown (Settings > Clock). The 3DS's own
 * menu adds an offset it keeps in its encrypted settings, which Aurora cannot
 * read, so Aurora keeps its own and never writes the RTC itself. */
extern int g_rtc_offset;

/* "HH:MM" into a buffer of at least 6 bytes. */
void rtc_format_time(const RtcTime *t, char *out);

/* "Tue 25 Aug" into a buffer of at least 11 bytes. */
void rtc_format_date(const RtcTime *t, char *out);

/* Battery charge as a percentage (0-100), or -1 if the MCU did not answer. */
int battery_percent(void);

/* 1 while the charger is supplying power, 0 when it is not, -1 on failure. */
int battery_charging(void);

/* Raw MCU power-status register, for diagnosing the charging bit. */
int power_status_raw(void);

/* Through the MCU; neither returns. */
void power_shutdown(void);
void power_reboot(void);

#endif
