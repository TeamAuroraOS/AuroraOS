#ifndef AURORA_POWER_H
#define AURORA_POWER_H

#include <stdint.h>

/* RTC and battery through the MCU (I2C device 3). Call I2C_init() first. */

typedef struct {
  int year;  /* full year, e.g. 2026 */
  int month; /* 1-12  */
  int day;   /* 1-31  */
  int hour;  /* 0-23  */
  int min;   /* 0-59  */
  int sec;   /* 0-59  */
  int wday;  /* 0 = Sunday */
} RtcTime;

/* Read the wall clock. Returns 1 on success, 0 if the MCU did not answer or
 * returned a value that cannot be a real date. */
int rtc_read(RtcTime *out);

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

#endif
