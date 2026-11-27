/* The OS's side of updates (src/os/Update.c) on the PC: restoring the backup,
 * finding the firm, and the clean-up at start, on the real FatFs over a RAM
 * disk. run.py links the UI calls Update.c makes to empty stubs; nothing here
 * reaches them. */
#include "ff.h"
#include "appnet.h"
#include "diskio.h"
#include "loader.h"
#include "nand.h"
#include "ui.h"
#include "update.h"

#include <bearssl_hash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

Font ui_small, ui_font, ui_bold, ui_title;
Color g_accent;
volatile u8 *g_fb_top, *g_fb_bot;

static int fails;
#define CHECK(what, cond)                                     \
  do {                                                        \
    int ok_ = !!(cond);                                       \
    printf("  %s %s\n", ok_ ? "ok  " : "FAIL", what);         \
    fails += !ok_;                                            \
  } while (0)

/* The card. */

static BYTE *disk;
static const LBA_t sectors = 64u * 2048u;

DSTATUS disk_status(BYTE pdrv) { return !pdrv && disk ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv || sector + count > sectors)
    return RES_PARERR;
  memcpy(buff, disk + sector * 512u, count * 512u);
  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv || sector + count > sectors)
    return RES_PARERR;
  memcpy(disk + sector * 512u, buff, count * 512u);
  return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  if (cmd == GET_SECTOR_COUNT)
    *(LBA_t *)buff = sectors;
  else if (cmd == GET_SECTOR_SIZE)
    *(WORD *)buff = 512;
  else if (cmd == GET_BLOCK_SIZE)
    *(DWORD *)buff = 1;
  return RES_OK;
}

static FATFS fs;

static void fresh_card(void) {
  static BYTE work[FF_MAX_SS * 8];
  static const MKFS_PARM opt = {FM_FAT32, 0, 0, 0, 0};
  f_mount(NULL, "", 0);
  free(disk);
  disk = calloc(sectors, 512u);
  if (f_mkfs("0:", &opt, work, sizeof(work)) != FR_OK) {
    printf("f_mkfs failed\n");
    exit(2);
  }
  f_mount(&fs, "", 1);
}

int appnet_mount(void) { return 1; }
static int g_battery = 80;
int battery_charging(void) { return 0; }
int battery_percent(void) { return g_battery; }

/* Nand.c's SHA-256, over BearSSL's: one hash at a time, as Update.c does. */
static br_sha256_context g_sha;
void sha256_init(Sha256 *s) { br_sha256_init(&g_sha); }
void sha256_update(Sha256 *s, const void *data, u32 len) {
  br_sha256_update(&g_sha, data, len);
}
void sha256_final(Sha256 *s, u8 out[32]) { br_sha256_out(&g_sha, out); }

/* AppNet as a scripted update server. */
static char g_asked[512];
static int g_status;
static const char *g_reply;
static const u8 *g_file;
static u32 g_file_len;
static char g_reply_buf[8192];

const char *appnet_error(void) { return "no route"; }

u32 appnet_url_encode(char *out, u32 max, const char *s) {
  u32 n = 0;
  for (; *s && n + 1 < max; s++)
    out[n++] = *s;
  out[n] = 0;
  return n;
}

int appnet_http(const char *method, const char *url, const char *headers,
                const void *body, u32 blen, AppNetReply *r) {
  snprintf(g_asked, sizeof(g_asked), "%s %s", method, url);
  snprintf(g_reply_buf, sizeof(g_reply_buf), "%s", g_reply ? g_reply : "");
  memset(r, 0, sizeof(*r));
  r->status = g_status;
  r->body = g_reply_buf;
  r->len = (u32)strlen(g_reply_buf);
  return g_status;
}

int appnet_download(const char *url, const char *headers, const char *path,
                    void (*progress)(u32 done, u32 total)) {
  FIL f;
  UINT bw;
  snprintf(g_asked, sizeof(g_asked), "GET %s -> %s", url, path);
  if (!g_file)
    return 0;
  f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
  f_write(&f, g_file, g_file_len, &bw);
  f_close(&f);
  if (progress)
    progress(g_file_len, g_file_len);
  return 1;
}

static void hex_of(const u8 *data, u32 len, char out[65]) {
  br_sha256_context c;
  u8 d[32];
  br_sha256_init(&c);
  br_sha256_update(&c, data, len);
  br_sha256_out(&c, d);
  for (int i = 0; i < 32; i++)
    sprintf(out + 2 * i, "%02x", d[i]);
}

static void mkdirs(const char *path) {
  char b[256];
  snprintf(b, sizeof(b), "%s", path);
  for (char *p = b + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      f_mkdir(b);
      *p = '/';
    }
}

static void put(const char *path, const char *text) {
  FIL f;
  UINT bw;
  mkdirs(path);
  f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
  f_write(&f, text, (UINT)strlen(text), &bw);
  f_close(&f);
}

/* The file's text, "" when it is not there. */
static const char *get(const char *path) {
  static char b[4][512];
  static int n;
  char *out = b[n++ % 4];
  FIL f;
  UINT br = 0;
  out[0] = 0;
  if (f_open(&f, path, FA_READ) == FR_OK) {
    f_read(&f, out, 511, &br);
    f_close(&f);
  }
  out[br] = 0;
  return out;
}

static int exists(const char *path) {
  FILINFO fno;
  return f_stat(path, &fno) == FR_OK;
}

static void firm_block(const char *path) {
  volatile u32 *magic = (volatile u32 *)(uintptr_t)AURORA_FIRM_INFO_ADDR;
  char *dst = (char *)(uintptr_t)(AURORA_FIRM_INFO_ADDR + 4u);
  *magic = path ? AURORA_FIRM_INFO_MAGIC : 0x12345678u;
  snprintf(dst, AURORA_FIRM_PATH_MAX, "%s", path ? path : "");
}

#define B UPDATE_BACKUP_DIR

static void restore(void) {
  char name[48], why[160];
  printf("restore\n");
  fresh_card();
  firm_block(NULL);
  put("/AURORAOS.BIN", "os v2");
  put("/Aurora/assets.pak", "assets v2");
  put(UPDATE_DEFAULT_FIRM, "firm v2");
  CHECK("no backup, nothing to restore", !update_backup(name, sizeof(name)));
  CHECK("restore refuses without one", !update_restore(why, sizeof(why)) &&
                                           strstr(why, "no AURORAOS.BIN"));
  put(B "/AURORAOS.BIN", "os v1");
  put(B "/Aurora.firm", "firm v1");
  put(B "/info.txt", "name=Beta 8 v0.1.7-everest\r\nfirm=" UPDATE_DEFAULT_FIRM
                     "\r\n");
  CHECK("the backup is found",
        update_backup(name, sizeof(name)) &&
            !strcmp(name, "Beta 8 v0.1.7-everest"));
  CHECK("restore works", update_restore(why, sizeof(why)));
  CHECK("the old OS is back", !strcmp(get("/AURORAOS.BIN"), "os v1"));
  CHECK("the old firm is back", !strcmp(get(UPDATE_DEFAULT_FIRM), "firm v1"));
  CHECK("assets.pak, not in the backup, stays",
        !strcmp(get("/Aurora/assets.pak"), "assets v2"));
  CHECK("the backup holds the newer files",
        !strcmp(get(B "/AURORAOS.BIN"), "os v2") &&
            !strcmp(get(B "/Aurora.firm"), "firm v2"));
  CHECK("info.txt names the version that was running",
        strstr(get(B "/info.txt"), "name=" AURORA_VERSION) &&
            strstr(get(B "/info.txt"), "firm=" UPDATE_DEFAULT_FIRM));
  CHECK("no swap files left", !exists(B "/AURORAOS.swp") &&
                                  !exists(B "/Aurora.swp"));
  CHECK("restoring again goes forward",
        update_restore(why, sizeof(why)) &&
            !strcmp(get("/AURORAOS.BIN"), "os v2") &&
            !strcmp(get(UPDATE_DEFAULT_FIRM), "firm v2"));
}

static void firm_path(void) {
  char why[160];
  printf("firm path\n");
  fresh_card();
  put("/AURORAOS.BIN", "os v2");
  put("/fw/Aurora.firm", "custom firm v2");
  put(UPDATE_DEFAULT_FIRM, "luma firm");
  put(B "/AURORAOS.BIN", "os v1");
  put(B "/Aurora.firm", "custom firm v1");
  put(B "/info.txt", "name=Beta 8\r\n");
  firm_block("/fw/Aurora.firm");
  CHECK("restore uses the path the firm reported",
        update_restore(why, sizeof(why)) &&
            !strcmp(get("/fw/Aurora.firm"), "custom firm v1") &&
            !strcmp(get(UPDATE_DEFAULT_FIRM), "luma firm"));
  CHECK("and records it", strstr(get(B "/info.txt"), "firm=/fw/Aurora.firm"));

  fresh_card();
  put("/AURORAOS.BIN", "os v2");
  put(UPDATE_DEFAULT_FIRM, "luma firm v2");
  put(B "/AURORAOS.BIN", "os v1");
  put(B "/Aurora.firm", "luma firm v1");
  put(B "/info.txt", "name=Beta 8\r\n");
  firm_block(NULL);
  CHECK("without a report, the Luma3DS payload",
        update_restore(why, sizeof(why)) &&
            !strcmp(get(UPDATE_DEFAULT_FIRM), "luma firm v1"));

  fresh_card();
  put("/AURORAOS.BIN", "os v2");
  put(UPDATE_DEFAULT_FIRM, "luma firm v2");
  put(B "/AURORAOS.BIN", "os v1");
  put(B "/Aurora.firm", "firm v1");
  put(B "/info.txt", "name=Beta 8\r\n");
  firm_block("/gone/Aurora.firm");
  CHECK("a reported firm that is not there falls back too",
        update_restore(why, sizeof(why)) &&
            !strcmp(get(UPDATE_DEFAULT_FIRM), "firm v1"));
}

static void at_boot(void) {
  const char *t = 0, *l = 0;
  printf("at boot\n");
  fresh_card();
  f_mount(NULL, "", 0);
  CHECK("no Update folder: nothing to say", !update_at_boot(&t, &l));
  f_mount(&fs, "", 1);
  put(UPDATE_APP, "updater");
  put(UPDATE_APP ".part", "half");
  put(UPDATE_PLAN, "current=x\r\n");
  put(UPDATE_DIR "/notes.txt", "notes");
  put(UPDATE_NEW_DIR "/AURORAOS.BIN", "half an OS");
  put(B "/AURORAOS.BIN", "os v1");
  put(UPDATE_RESULT, "ok=1\r\nto=Beta 9 v0.1.8\r\n");
  f_mount(NULL, "", 0);
  CHECK("the result is reported", update_at_boot(&t, &l) &&
                                      !strcmp(t, "Updated to Beta 9 v0.1.8") &&
                                      strstr(l, "restore"));
  f_mount(&fs, "", 1);
  CHECK("the updater and its downloads are gone",
        !exists(UPDATE_APP) && !exists(UPDATE_APP ".part") &&
            !exists(UPDATE_PLAN) && !exists(UPDATE_NEW_DIR) &&
            !exists(UPDATE_RESULT) && !exists(UPDATE_DIR "/notes.txt"));
  CHECK("the backup stays", !strcmp(get(B "/AURORAOS.BIN"), "os v1"));
  f_mount(NULL, "", 0);
  CHECK("said once", !update_at_boot(&t, &l));
  f_mount(&fs, "", 1);
  put(UPDATE_RESULT, "ok=0\r\nto=Beta 9 v0.1.8\r\nwhy=A file could not be "
                     "moved.\r\n");
  f_mount(NULL, "", 0);
  CHECK("a failure is reported with its reason",
        update_at_boot(&t, &l) && !strcmp(t, "The update did not finish") &&
            !strcmp(l, "A file could not be moved."));
  f_mount(&fs, "", 1);
  put(UPDATE_RESULT, "ok=1\r\nto=Beta 9\r\nnote=The firm was not updated.\r\n");
  f_mount(NULL, "", 0);
  CHECK("a success note is shown",
        update_at_boot(&t, &l) && !strcmp(l, "The firm was not updated."));
}

/* The shape aurora-site/Update sends (src/manifest.ts latestReply). */
static void check_and_fetch(void) {
  static UpdateInfo u;
  static u8 app[36 + 400];
  static char reply[4096];
  char why[160], hex[65], want[160];
  printf("check and fetch\n");
  fresh_card();
  memset(app, 0x5A, sizeof(app));
  memcpy(app, "AUR1", 4);
  u32 hdr[8] = {36, 400, 0x22000000u, 0x22000000u, 0, 0, 0, 0};
  memcpy(app + 4, hdr, sizeof(hdr));
  hex_of(app, sizeof(app), hex);
  snprintf(reply, sizeof(reply),
           "{\"update\":true,\"current\":\"0.1.7-everest\","
           "\"name\":\"Beta 9 v0.1.8\",\"version\":\"0.1.8\","
           "\"tag\":\"Beta-9-v0.1.8\",\"published\":\"2026-12-01\","
           "\"prerelease\":true,\"size\":2500000,"
           "\"notes\":\"Beta 9\\nNew Features:\\n- Camera: caf\\u00e9\","
           "\"updater\":{\"url\":\"https://update.aurora3ds.xyz/v1/files/%s\","
           "\"sha256\":\"%s\",\"size\":%u},"
           "\"files\":[{\"name\":\"AURORAOS.BIN\",\"kind\":\"os\","
           "\"path\":\"/AURORAOS.BIN\",\"sha256\":\"%s\",\"size\":1,"
           "\"url\":\"https://update.aurora3ds.xyz/v1/files/%s\"}]}",
           hex, hex, (unsigned)sizeof(app), hex, hex);
  g_status = 200;
  g_reply = reply;
  CHECK("update_check reads the server's reply",
        update_check(&u, why, sizeof(why)) && u.update &&
            !strcmp(u.name, "Beta 9 v0.1.8") &&
            !strcmp(u.published, "2026-12-01") && u.size == 2500000u &&
            u.app_size == sizeof(app) && !strcmp(u.app_sha256, hex) &&
            !strncmp(u.app_url, "https://update.aurora3ds.xyz/v1/files/", 38));
  CHECK("the notes keep their lines and Latin-1",
        !strcmp(update_notes(), "Beta 9\nNew Features:\n- Camera: caf\xc3\xa9"));
  snprintf(want, sizeof(want),
           "GET " UPDATE_SERVER UPDATE_LATEST "?current=" AURORA_VERSION_NUM
           "&build=%u",
           (unsigned)AURORA_BUILD_DATE);
  CHECK("it asks with this version and build", !strcmp(g_asked, want));

  firm_block("/fw/Aurora.firm");
  put("/fw/Aurora.firm", "firm");
  g_file = app;
  g_file_len = sizeof(app);
  CHECK("update_fetch downloads and checks the updater",
        update_fetch(&u, 0, why, sizeof(why)) && exists(UPDATE_APP));
  const char *plan = get(UPDATE_PLAN);
  CHECK("plan.txt carries the version and the firm's path",
        strstr(plan, "current=" AURORA_VERSION_NUM "\r\n") &&
            strstr(plan, "from=" AURORA_VERSION "\r\n") &&
            strstr(plan, "firm=/fw/Aurora.firm\r\n") &&
            strstr(plan, "build=2"));

  f_unlink(UPDATE_APP);
  app[100] ^= 1;
  CHECK("a damaged updater is refused and removed",
        !update_fetch(&u, 0, why, sizeof(why)) && !exists(UPDATE_APP) &&
            strstr(why, "did not download correctly"));
  app[100] ^= 1;
  g_battery = 12;
  CHECK("a low battery stops it before the download",
        !update_fetch(&u, 0, why, sizeof(why)) && strstr(why, "Charge"));
  g_battery = 80;

  g_reply = "{\"update\":false,\"name\":\"Beta 8 v0.1.7-everest\"}";
  CHECK("up to date", update_check(&u, why, sizeof(why)) && !u.update &&
                          !strcmp(u.name, "Beta 8 v0.1.7-everest"));
  g_reply = "{\"update\":true,\"name\":\"Beta 9\"}";
  CHECK("a reply without the updater is refused",
        !update_check(&u, why, sizeof(why)) && strstr(why, "incomplete"));
  g_status = 503;
  g_reply = "{\"error\":\"not_ready\"}";
  CHECK("503: nothing ready yet",
        !update_check(&u, why, sizeof(why)) && strstr(why, "No release"));
  g_status = 0;
  CHECK("no answer", !update_check(&u, why, sizeof(why)) &&
                         !strcmp(why, "no route"));
}

int main(void) {
  void *p = mmap((void *)(uintptr_t)0x25000000u, 0x10000u,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (p != (void *)(uintptr_t)0x25000000u) {
    printf("cannot map 0x25000000\n");
    return 2;
  }
  restore();
  firm_path();
  at_boot();
  check_and_fetch();
  printf("\n%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
