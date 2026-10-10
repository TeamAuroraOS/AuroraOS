/* Shared by the SDK runtime's sources; apps never see it. The runtime is
 * built from these files plus the OS's own drivers (screen.c, Gpu9.c,
 * Touch9.c, FatFs and the SD driver, the image and WAV readers, crash.c),
 * with OS headers first: aurora.h names its own COLOR_* values. */
#ifndef AURORA_APP_INTERNAL_H
#define AURORA_APP_INTERNAL_H

#include "aurora.h"
#define AURORA_APP_NO_COLORS
#include "aurora_app.h"

/* Where the stdio file descriptors start: 0..2 are the console. */
#define APP_FD_FIRST 3
#define APP_FD_COUNT 16

extern bool g_app_leaving;
bool app_core(void);
bool app_home_pressed(void);
/* Leaves through exit() when HOME was pressed. */
void app_poll_home(void);
void app_leave(int code) __attribute__((noreturn));
/* Posts a command to the ARM11 core and waits until it is taken. */
void app_core_post(u32 cmd, u32 a0, u32 a1, u32 a2, u32 a3);
/* USER.dat's 64 bytes, or NULL without a valid one. */
const u8 *app_user_dat(void);
void app_flush_stdout(void);

void app_gfx_init(void);
/* Shows the screens drawn on since the last present; returns them as a mask,
 * bit 0 the top and bit 1 the bottom. */
u32 app_present(void);
volatile u8 *app_fb(Screen s);
void app_dirty(Screen s);
/* Presents by the CPU to framebuffer A, which is put on show, while the
 * core runs a network call. */
void app_gfx_direct(bool on);

void app_hid_init(void);

void app_con_write(const char *p, size_t n);

void app_snd_leave(void);

/* A network call is with the core: nothing else may be posted to it. */
bool app_net_busy(void);
/* Waits for such a call before the app leaves. */
void app_net_leave(void);

/* The C library's file calls: negative errno values on failure. */
bool app_fs_mount(void);
/* Any path the SDK accepts as FatFs's "0:/...", "." and ".." resolved; one
 * without a leading "/" or drive starts at the working directory. */
bool app_fs_path(const char *in, char *out, size_t max);
int app_fs_open(const char *path, int flags);
int app_fs_close(int fd);
int app_fs_read(int fd, void *buf, size_t n);
int app_fs_write(int fd, const void *buf, size_t n);
long app_fs_lseek(int fd, long off, int whence);
int app_fs_fstat(int fd, void *st);
int app_fs_stat(const char *path, void *st);
int app_fs_unlink(const char *path);
int app_fs_rmdir(const char *path);
int app_fs_rename(const char *from, const char *to);
int app_fs_mkdir(const char *path);
int app_fs_ftruncate(int fd, long len);
int app_fs_fsync(int fd);
int app_fs_chdir(const char *path);
/* "/" or "/Aurora/...". */
const char *app_fs_getcwd(void);
void app_fs_close_all(void);

u64 app_ticks64(void);
void app_wait_ticks(u64 ticks);
/* Seconds since 1970 for a date and time on the console's clock. */
long long app_civil_secs(int year, int month, int day, int hour, int min,
                         int sec);

static inline Color app_color(uint32_t c) {
  Color k;
  k.r = (u8)(c >> 16);
  k.g = (u8)(c >> 8);
  k.b = (u8)c;
  return k;
}

#endif
