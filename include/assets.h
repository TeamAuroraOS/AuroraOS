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

/* As assets_load(), on a card the caller has already mounted and keeps. */
int assets_read(void);

/* Null when the pack is absent or `id` is out of range. */
const Asset *asset_get(uint32_t id);

/* Decodes sound `id` (ASSET_SOUND_*) into `dst` as signed 16-bit mono, on a
 * card the caller has mounted, and gives its rate. Sounds stay on the card
 * until asked for. Returns the sample count; 0 when the pack has no such
 * sound, it is longer than `max` samples, or it does not read back. */
uint32_t asset_sound_read(uint32_t id, int16_t *dst, uint32_t max,
                          uint32_t *rate);

/* A long sound decoded a slice at a time, so that it does not hold up the
 * caller's screen. */
typedef struct {
  const uint8_t *src;
  int16_t *dst;
  int16_t coef[16];
  int32_t h1, h2;
  uint32_t total, done;
} AssetSoundStream;

/* Reads sound `id` packed into `buf`, on a card the caller has mounted, and
 * sets up `s` to decode it into `dst`; the card is not needed after this.
 * Returns the sample count, or 0 as asset_sound_read() does, also when the
 * packed sound does not fit in `buf_bytes`. */
uint32_t asset_sound_open(uint32_t id, void *buf, uint32_t buf_bytes,
                          int16_t *dst, uint32_t max, AssetSoundStream *s,
                          uint32_t *rate);

/* Decodes up to `frames` more frames (14 samples each). The sound is finished
 * when s->done == s->total. Returns 0 on a damaged frame. */
int asset_sound_decode(AssetSoundStream *s, uint32_t frames);

/* 0 if unavailable. */
int asset_font(uint32_t id, Font *out);

const char *assets_error(void);

#endif
