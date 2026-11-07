/* The runtime calls that live in files the harness does not build
 * (app_main.c, app_sys.c, app_gfx.c, app_hid.c), reduced to what the tests
 * need: the app started from the Home Menu, no HOME press, app time on the
 * simulated clock, and a lid tests can close. */
#include "app_internal.h"
#include "hostrt.h"
#include "model.h"

const AppSockOps *g_app_sock;
bool g_app_leaving;
uint32_t g_host_lid_us;
uint32_t g_host_waiting_calls;

bool app_from_home(void) { return true; }
bool app_home_pressed(void) { return false; }
void app_hid_home_menu_poll(void) {}
void crash_poll_arm11(void) {}
void app_gfx_direct(bool on) { (void)on; }

void app_poll_home(void) { app_sock_tick(); }

/* Time passes, and the core gets on, whenever the app reads the clock: a
 * loop that only polls the rings and the clock still ends. */
static u64 now(void) {
  host_advance(1);
  model_step();
  return host_now_us();
}

u64 app_raw_ticks64(void) { return now(); }
u64 app_ticks64(void) { return now(); }
uint32_t sys_millis(void) { return (uint32_t)(now() / 1000u); }

/* While "closed", the app keeps its connection alive as app_sys.c does. */
bool app_lid_wait(void) {
  if (!g_host_lid_us)
    return false;
  for (uint64_t end = host_now_us() + g_host_lid_us; host_now_us() < end;) {
    app_sock_tick();
    delay(60000);
  }
  g_host_lid_us = 0;
  return true;
}

void app_wait_ticks(u64 ticks) {
  for (u64 end = host_now_us() + ticks; host_now_us() < end;) {
    app_poll_home();
    delay(2000);
  }
}

void host_frames(uint32_t n) {
  while (n--)
    app_wait_ticks(16667u);
}

/* app_sys.c's, for app_fs.c's file times. */
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
