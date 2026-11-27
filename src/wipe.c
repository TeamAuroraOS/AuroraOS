/* Settings > Wipe Aurora (docs/power.md, "Wiping Aurora"). Folders are
 * emptied the way FatFs's own delete_node() example does it: items are
 * removed while the folder is still being read. A read-only item would make
 * f_unlink() refuse, so that bit is cleared first. */

#include "wipe.h"
#include "ff.h"
#include "loader.h"

#define DEFAULT_FIRM "/luma/payloads/Aurora.firm"

static FATFS wipe_fs;
static FILINFO wipe_fno; /* one for every level: read before going deeper */
static char wipe_buf[WIPE_PATH_MAX];

static WipeStatus *g_st;
static void (*g_tick)(const WipeStatus *, void *);
static void *g_ctx;

int wipe_firm_known(void) { return loader_firm_path()[0] != 0; }

const char *wipe_firm_path(void) {
  return wipe_firm_known() ? loader_firm_path() : DEFAULT_FIRM;
}

static u32 str_len(const char *s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}

static void str_copy(char *d, const char *s, u32 max) {
  u32 n = 0;
  while (s[n] && n < max - 1u) {
    d[n] = s[n];
    n++;
  }
  d[n] = 0;
}

static void tick(const char *path) {
  g_st->current = path;
  if (g_tick)
    g_tick(g_st, g_ctx);
}

static FRESULT fail(const char *path, FRESULT fr) {
  str_copy(g_st->where, path, WIPE_PATH_MAX);
  g_st->fr = (int)fr;
  return fr;
}

static FRESULT remove_one(const char *path, BYTE attr) {
  FRESULT fr;
  if (attr & AM_RDO)
    f_chmod(path, 0, AM_RDO);
  fr = f_unlink(path);
  if (fr != FR_OK)
    return fail(path, fr);
  g_st->deleted++;
  tick(path);
  return FR_OK;
}

/* `path` (in wipe_buf, `len` long) is a folder; it is emptied, then removed. */
static FRESULT remove_tree(u32 len, BYTE attr) {
  DIR dir;
  FRESULT fr = f_opendir(&dir, wipe_buf);
  if (fr != FR_OK)
    return fail(wipe_buf, fr);

  wipe_buf[len] = '/';
  for (;;) {
    u32 n;
    fr = f_readdir(&dir, &wipe_fno);
    if (fr != FR_OK) {
      wipe_buf[len] = 0;
      fail(wipe_buf, fr);
      break;
    }
    if (!wipe_fno.fname[0])
      break;
    n = str_len(wipe_fno.fname);
    if (len + 1u + n >= WIPE_PATH_MAX) {
      str_copy(wipe_buf + len + 1u, wipe_fno.fname, WIPE_PATH_MAX - len - 1u);
      fr = fail(wipe_buf, FR_INVALID_NAME);
      break;
    }
    str_copy(wipe_buf + len + 1u, wipe_fno.fname, n + 1u);
    if (wipe_fno.fattrib & AM_DIR)
      fr = remove_tree(len + 1u + n, wipe_fno.fattrib);
    else
      fr = remove_one(wipe_buf, wipe_fno.fattrib);
    if (fr != FR_OK)
      break;
  }
  wipe_buf[len] = 0;
  f_closedir(&dir);
  if (fr == FR_OK)
    fr = remove_one(wipe_buf, attr);
  return fr;
}

/* Anything already gone counts as deleted. */
static FRESULT remove_path(const char *path) {
  FILINFO fno;
  FRESULT fr = f_stat(path, &fno);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH)
    return FR_OK;
  if (fr != FR_OK)
    return fail(path, fr);
  if (!(fno.fattrib & AM_DIR))
    return remove_one(path, fno.fattrib);
  str_copy(wipe_buf, path, WIPE_PATH_MAX);
  return remove_tree(str_len(wipe_buf), fno.fattrib);
}

static int step(int which, const char *path) {
  g_st->step = which;
  tick(path);
  return remove_path(path) == FR_OK;
}

int wipe_run(WipeStatus *st, void (*cb)(const WipeStatus *, void *),
             void *ctx) {
  FRESULT fr;
  FILINFO fno;
  int ok = 0;

  st->step = WIPE_FOLDER;
  st->deleted = 0;
  st->fr = 0;
  st->firm_missing = 0;
  st->current = "";
  st->where[0] = 0;
  g_st = st;
  g_tick = cb;
  g_ctx = ctx;

  fr = f_mount(&wipe_fs, "", 1);
  if (fr != FR_OK) {
    fail("SD card", fr);
    return 0;
  }
  if (step(WIPE_FOLDER, "/Aurora") && step(WIPE_OS, "/AURORAOS.BIN")) {
    const char *firm = wipe_firm_path();
    g_st->step = WIPE_FIRM;
    tick(firm);
    fr = f_stat(firm, &fno);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
      st->firm_missing = 1;
      ok = 1;
    } else if (fr != FR_OK) {
      fail(firm, fr);
    } else {
      ok = remove_one(firm, fno.fattrib) == FR_OK;
    }
  }
  if (ok) {
    st->step = WIPE_DONE;
    tick("");
  }
  f_mount(NULL, "", 0);
  return ok;
}
