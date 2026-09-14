#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>
#include "asset_ids.h"

/* Art and fonts from SD:\Aurora\assets.pak (tools/mkassets.py), stored at the
 * exact size they are drawn. Icons are coverage masks tinted at draw time.
 * Without the pack, callers fall back to the built-in icons and 8x8 font. */

/* Decoded assets sit clear of the bottom backbuffer (which ends by 0x23F38400)
 * and the app-launch staging area at 0x24000000. */
#define ASSETS_ARENA_ADDR 0x23F40000u
#define ASSETS_ARENA_MAX  0x000C0000u

typedef enum {
  ASSET_TYPE_ALPHA8 = 0, /* w*h coverage bytes                     */
  ASSET_TYPE_RGBA   = 1, /* w*h*4, colour with straight alpha      */
  ASSET_TYPE_FONT   = 2, /* AssetFontHead, glyph table, then atlas */
} AssetType;

typedef struct Asset {
  uint16_t type, w, h;
  const uint8_t *data;
} Asset;

/* 10 bytes, laid out to match the record tools/mkassets.py writes. */
typedef struct {
  int8_t left, top; /* placement from the pen, top counted upward */
  uint8_t w, h, adv, pad;
  uint16_t ax, ay;  /* where the glyph sits in the atlas */
} AssetGlyph;

typedef struct {
  uint16_t first, count;
  int16_t ascent, descent, line_height;
  uint16_t atlas_w, atlas_h, pad;
} AssetFontHead;

typedef struct Font {
  const AssetFontHead *head;
  const AssetGlyph *glyphs;
  const uint8_t *atlas;
} Font;

/* Mounts the SD card, decodes the pack into the arena, unmounts. Safe to call
 * more than once; the second call is a no-op. Returns 1 on success. */
int assets_load(void);
int assets_ok(void);

/* Null when the pack is absent or `id` is out of range. */
const Asset *asset_get(uint32_t id);

/* Resolves a font asset into `out`. Returns 0 if unavailable. */
int asset_font(uint32_t id, Font *out);

const char *assets_error(void);

#endif
