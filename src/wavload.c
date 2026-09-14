/* RIFF/WAVE reader, separate from wav_play() so Auric apps can link it. 8-bit
 * samples are biased to signed and stereo is mixed down, a block at a time
 * straight into the destination. */

#include "wav.h"
#include "ff.h"

/* A multiple of the sector size, so FatFs reads straight into it. */
#define CHUNK 32768

static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int wav_sample(const u8 *s, u32 depth, u32 chans) {
  if (depth == 8) {
    int v = s[0] - 128; /* WAV 8-bit is unsigned, CSND wants signed */
    if (chans == 2)
      v = (v + (int)s[1] - 128) / 2;
    return v;
  }
  int l = (int)(s16)(u16)(s[0] | (s[1] << 8));
  if (chans == 2) {
    int r = (int)(s16)(u16)(s[2] | (s[3] << 8));
    l = (l + r) / 2;
  }
  return l;
}

WavResult wav_load(const char *path, u8 *dst, u32 max, int halve,
                   WavInfo *info) {
  static FIL f;
  static u8 buf[CHUNK];
  UINT br;
  u8 hdr[12];
  u32 chans = 0, rate = 0, depth = 0, fmt = 0;
  u32 data_len = 0;
  int have_fmt = 0;

  if (f_open(&f, path, FA_READ) != FR_OK)
    return WAV_ERR_OPEN;
  if (f_read(&f, hdr, 12, &br) != FR_OK || br != 12) {
    f_close(&f);
    return WAV_ERR_OPEN;
  }
  if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F' ||
      hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E') {
    f_close(&f);
    return WAV_ERR_FORMAT;
  }

  /* Walk the chunk list; "fmt " and "data" can be in either order and other
   * chunks (LIST, fact, cue) sit between them. */
  for (;;) {
    u8 ch[8];
    if (f_read(&f, ch, 8, &br) != FR_OK || br != 8) {
      f_close(&f);
      return have_fmt ? WAV_ERR_DATA : WAV_ERR_FORMAT;
    }
    u32 clen = rd32(ch + 4);
    if (ch[0] == 'f' && ch[1] == 'm' && ch[2] == 't' && ch[3] == ' ') {
      u8 fm[16];
      if (clen < 16 || f_read(&f, fm, 16, &br) != FR_OK || br != 16) {
        f_close(&f);
        return WAV_ERR_DATA;
      }
      fmt = rd16(fm);
      chans = rd16(fm + 2);
      rate = rd32(fm + 4);
      depth = rd16(fm + 14);
      have_fmt = 1;
      if (clen > 16 && f_lseek(&f, f_tell(&f) + (clen - 16)) != FR_OK) {
        f_close(&f);
        return WAV_ERR_DATA;
      }
    } else if (ch[0] == 'd' && ch[1] == 'a' && ch[2] == 't' && ch[3] == 'a') {
      data_len = clen;
      break;
    } else {
      if (f_lseek(&f, f_tell(&f) + clen + (clen & 1u)) != FR_OK) {
        f_close(&f);
        return WAV_ERR_DATA;
      }
    }
  }

  if (!have_fmt || !data_len) {
    f_close(&f);
    return WAV_ERR_DATA;
  }
  /* 0xFFFE is WAVE_FORMAT_EXTENSIBLE; its sub-format is not read, so it is only
   * accepted at the PCM depths CSND plays. */
  if ((fmt != 1 && fmt != 0xFFFE) || (depth != 8 && depth != 16) || chans < 1 ||
      chans > 2 || rate < 1000 || rate > 96000) {
    f_close(&f);
    return WAV_ERR_UNSUPPORTED;
  }

  u32 frame = (depth / 8u) * chans;              /* bytes per sample frame  */
  u32 out_bpp = depth / 8u;                      /* bytes per mono sample   */
  u32 step = (halve && rate > 32000u) ? 2u : 1u; /* frames per output sample */
  u32 fit = (CHUNK / frame / step) * step;       /* frames per read         */
  u32 samples = (data_len / frame) / step;
  if (samples * out_bpp > max)
    samples = max / out_bpp;

  u8 *out = dst;
  u32 done = 0;
  while (done < samples) {
    u32 want = (samples - done) * step;
    if (want > fit)
      want = fit;
    if (f_read(&f, buf, want * frame, &br) != FR_OK || br == 0)
      break;
    u32 got = (br / frame) / step;
    for (u32 i = 0; i < got; i++) {
      int v = wav_sample(buf + i * step * frame, depth, chans);
      if (step == 2u)
        v = (v + wav_sample(buf + (i * 2u + 1u) * frame, depth, chans)) / 2;
      *out++ = (u8)(v & 0xFF);
      if (depth == 16)
        *out++ = (u8)((v >> 8) & 0xFF);
    }
    done += got;
    if (br < want * frame)
      break;
  }
  f_close(&f);

  if (!done)
    return WAV_ERR_DATA;
  info->samples = done;
  info->rate = rate / step;
  info->depth = depth;
  return WAV_OK;
}
