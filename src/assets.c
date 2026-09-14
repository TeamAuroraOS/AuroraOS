/* Reads SD:\Aurora\assets.pak and RLE-decodes every entry into a fixed arena.
 * All-or-nothing: any inconsistency leaves assets_ok() false. Layout in
 * docs/assets.md. */

#include "assets.h"
#include "aurora.h"
#include "ff.h"

#define PAK_MAGIC   0x4B415041u /* "APAK" */
#define PAK_VERSION 1u
#define PAK_PATH    "0:/Aurora/assets.pak"

typedef struct {
  uint32_t magic, version, count, data_off, arena, reserved[3];
} PakHead;

typedef struct {
  uint32_t id;
  uint16_t type, w, h, pad;
  uint32_t off, csize, usize;
} PakEntry;

/* The pack is written by a Python script packing raw little-endian fields, so
 * any padding the compiler adds here would silently misread every entry. */
typedef char assets_layout_check[
    (sizeof(PakHead) == 32 && sizeof(PakEntry) == 24 &&
     sizeof(AssetFontHead) == 16 && sizeof(AssetGlyph) == 10 &&
     ASSET_ARENA_NEEDED <= ASSETS_ARENA_MAX)
        ? 1
        : -1];

static Asset g_assets[ASSET_COUNT];
static int g_ok;
static const char *g_err = "not loaded";

static PakEntry g_table[ASSET_COUNT];
static uint8_t g_scratch[ASSET_MAX_CSIZE];

/* 0x01..0x7F: run of n copies of the next byte. 0x81..0xFF: n literals. */
static void rle_decode(const uint8_t *src, uint32_t csize, uint8_t *dst,
                       uint32_t usize) {
  const uint8_t *send = src + csize;
  uint8_t *out = dst, *oend = dst + usize;
  while (src < send && out < oend) {
    uint8_t c = *src++;
    if (c & 0x80) {
      uint32_t n = c & 0x7Fu;
      while (n-- && out < oend && src < send)
        *out++ = *src++;
    } else {
      uint32_t n = c;
      uint8_t v;
      if (src >= send)
        break;
      v = *src++;
      while (n-- && out < oend)
        *out++ = v;
    }
  }
}

int assets_ok(void) { return g_ok; }
const char *assets_error(void) { return g_err; }

const Asset *asset_get(uint32_t id) {
  if (!g_ok || id >= ASSET_COUNT || !g_assets[id].data)
    return 0;
  return &g_assets[id];
}

int asset_font(uint32_t id, Font *out) {
  const Asset *a = asset_get(id);
  const AssetFontHead *h;
  if (!a || a->type != ASSET_TYPE_FONT)
    return 0;
  h = (const AssetFontHead *)a->data;
  out->head = h;
  out->glyphs = (const AssetGlyph *)(a->data + sizeof(AssetFontHead));
  out->atlas = a->data + sizeof(AssetFontHead) + h->count * sizeof(AssetGlyph);
  return 1;
}

static int fail(FATFS *fs, const char *why) {
  f_mount(NULL, "", 0);
  (void)fs;
  g_err = why;
  g_ok = 0;
  return 0;
}

int assets_load(void) {
  static FATFS fs;
  static FIL f;
  PakHead head;
  UINT br;
  uint32_t i, used = 0;
  uint8_t *arena = (uint8_t *)ASSETS_ARENA_ADDR;

  if (g_ok)
    return 1;

  if (f_mount(&fs, "", 1) != FR_OK)
    return fail(&fs, "no SD card");
  if (f_open(&f, PAK_PATH, FA_READ) != FR_OK)
    return fail(&fs, "assets.pak not found");

  if (f_read(&f, &head, sizeof(head), &br) != FR_OK || br != sizeof(head)) {
    f_close(&f);
    return fail(&fs, "pack header unreadable");
  }
  if (head.magic != PAK_MAGIC || head.version != PAK_VERSION) {
    f_close(&f);
    return fail(&fs, "pack magic/version mismatch");
  }
  if (head.count != ASSET_COUNT) {
    f_close(&f);
    return fail(&fs, "pack does not match this build");
  }
  if (head.arena > ASSETS_ARENA_MAX) {
    f_close(&f);
    return fail(&fs, "pack too large for the arena");
  }

  if (f_read(&f, g_table, sizeof(PakEntry) * head.count, &br) != FR_OK ||
      br != sizeof(PakEntry) * head.count) {
    f_close(&f);
    return fail(&fs, "pack index unreadable");
  }

  for (i = 0; i < head.count; i++) {
    const PakEntry *e = &g_table[i];
    if (e->id >= ASSET_COUNT || e->csize > ASSET_MAX_CSIZE ||
        used + e->usize > ASSETS_ARENA_MAX) {
      f_close(&f);
      return fail(&fs, "pack entry out of range");
    }
    if (f_lseek(&f, e->off) != FR_OK ||
        f_read(&f, g_scratch, e->csize, &br) != FR_OK || br != e->csize) {
      f_close(&f);
      return fail(&fs, "pack payload unreadable");
    }
    rle_decode(g_scratch, e->csize, arena + used, e->usize);
    g_assets[e->id].type = e->type;
    g_assets[e->id].w = e->w;
    g_assets[e->id].h = e->h;
    g_assets[e->id].data = arena + used;
    used += (e->usize + 3u) & ~3u;
  }

  f_close(&f);
  f_mount(NULL, "", 0);
  g_err = "";
  g_ok = 1;
  return 1;
}

/* Returns 0 when the asset is missing, so callers can fall back. Lives here
 * rather than in screen.c because Auric apps link screen.c without the loader. */
int draw_asset_boxed(volatile u8 *fb, int bx, int by, int bw, int bh,
                     int screen_height, uint32_t id, Color tint) {
  const Asset *a = asset_get(id);
  if (!a)
    return 0;
  draw_asset(fb, bx + (bw - (int)a->w) / 2, by + (bh - (int)a->h) / 2,
             screen_height, a, tint);
  return 1;
}
