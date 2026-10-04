/* The system NAND, read only.
 *
 * The NAND starts with an NCSD header (unencrypted) whose partition table
 * gives CTRNAND in 512-byte units, with its crypt type: 2 on an Old 3DS
 * (AES keyslot 0x04) and 3 on a New 3DS (keyslot 0x05). The bootrom leaves
 * both keyXs and the 0x04 keyY; the 0x05 keyY is the value New 3DS firmware
 * sets, the same one Luma3DS's crypto.c (GPL-3.0) sets before reading
 * CTRNAND. A wrong key cannot pass unnoticed: CTRNAND must decrypt to an MBR.
 *
 * The CTR counter is the first 16 bytes of SHA-256(eMMC CID) plus the
 * sector's byte offset in the NAND divided by 16 (GodMode9, Luma3DS). */

#include "nand.h"
#include "aes.h"
#include "sdmmc.h"
#include <string.h>

static const u8 n3ds_ctrnand_keyy[16] = {0x4D, 0x80, 0x4F, 0x4E, 0x99, 0x90,
                                         0x19, 0x46, 0x13, 0xA2, 0x04, 0xAC,
                                         0x58, 0x44, 0x60, 0xBE};

static u8 ctr_base[16];
static u32 part_start;
static int part_slot;
static u32 bounce[128]; /* one sector, word aligned */

static u32 rd32(const u8 *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

/* FatFs drive 1. The engine works on whole words, so a buffer that is not
 * word aligned goes a sector at a time through `bounce`. */
static int ctrnand_read(u32 sector, u32 count, u8 *out) {
  u8 ctr[16];
  if ((u32)out & 3u) {
    for (u32 i = 0; i < count; i++) {
      if (ctrnand_read(sector + i, 1, (u8 *)bounce))
        return -1;
      memcpy(out + i * 512u, bounce, 512);
    }
    return 0;
  }
  while (count) {
    u32 n = count > 128u ? 128u : count;
    if (sdmmc_nand_readsectors(part_start + sector, n, out))
      return -1;
    memcpy(ctr, ctr_base, 16);
    aes_ctr_add(ctr, (part_start + sector) * 32u);
    aes_ctr(part_slot, ctr, out, n * 512u);
    sector += n;
    out += n * 512u;
    count -= n;
  }
  return 0;
}

int nand_open(void) {
  const u8 *h = (const u8 *)bounce;
  u8 cid[16], hash[32];
  u32 start = 0, size = 0;
  int crypt = 0;

  nand_close();
  if (sdmmc_nand_init() || sdmmc_nand_readsectors(0, 1, (u8 *)bounce))
    return NAND_ERR_EMMC;
  if (memcmp(h + 0x100, "NCSD", 4))
    return NAND_ERR_NCSD;
  for (int i = 0; i < 8 && !size; i++) {
    u8 fs = h[0x110 + i], ct = h[0x118 + i];
    if (fs == 1 && (ct == 2 || ct == 3) && rd32(h + 0x124 + i * 8)) {
      start = rd32(h + 0x120 + i * 8);
      size = rd32(h + 0x124 + i * 8);
      crypt = ct;
    }
  }
  if (!size)
    return NAND_ERR_NCSD;
  if (!sdmmc_nand_cid(cid))
    return NAND_ERR_EMMC;

  part_start = start;
  part_slot = crypt == 3 ? 0x05 : 0x04;
  if (part_slot == 0x05)
    aes_keyy(0x05, n3ds_ctrnand_keyy);
  sha256(cid, 16, hash);
  memcpy(ctr_base, hash, 16);

  if (ctrnand_read(0, 1, (u8 *)bounce))
    return NAND_ERR_EMMC;
  if (h[510] != 0x55 || h[511] != 0xAA || !h[0x1C2])
    return NAND_ERR_KEYS;
  g_nand_sectors = size;
  g_nand_read = ctrnand_read;
  return NAND_OK;
}

void nand_close(void) {
  g_nand_read = 0;
  g_nand_sectors = 0;
}

const char *nand_error(int err) {
  switch (err) {
    case NAND_OK:       return "";
    case NAND_ERR_EMMC: return "The system NAND could not be read";
    case NAND_ERR_NCSD: return "The system NAND has no partition table";
    default:            return "The system NAND did not decrypt";
  }
}

static const u32 sha_k[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1,
    0x923F82A4, 0xAB1C5ED5, 0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174, 0xE49B69C1, 0xEFBE4786,
    0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147,
    0x06CA6351, 0x14292967, 0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85, 0xA2BFE8A1, 0xA81A664B,
    0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A,
    0x5B9CCA4F, 0x682E6FF3, 0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(u32 *st, const u8 *b) {
  u32 w[64], a, bb, c, d, e, f, g, hh;
  for (int i = 0; i < 16; i++)
    w[i] = ((u32)b[i * 4] << 24) | ((u32)b[i * 4 + 1] << 16) |
           ((u32)b[i * 4 + 2] << 8) | b[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = st[0]; bb = st[1]; c = st[2]; d = st[3];
  e = st[4]; f = st[5]; g = st[6]; hh = st[7];
  for (int i = 0; i < 64; i++) {
    u32 t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) +
             sha_k[i] + w[i];
    u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
             ((a & bb) ^ (a & c) ^ (bb & c));
    hh = g; g = f; f = e; e = d + t1;
    d = c; c = bb; bb = a; a = t1 + t2;
  }
  st[0] += a; st[1] += bb; st[2] += c; st[3] += d;
  st[4] += e; st[5] += f; st[6] += g; st[7] += hh;
}

void sha256(const void *data, u32 len, u8 out[32]) {
  u32 st[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
               0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
  const u8 *p = (const u8 *)data;
  u8 tail[128];
  u32 n = len, rest, pad;

  while (n >= 64) {
    sha_block(st, p);
    p += 64;
    n -= 64;
  }
  rest = n;
  memcpy(tail, p, rest);
  tail[rest] = 0x80;
  pad = rest < 56 ? 64 : 128;
  memset(tail + rest + 1, 0, pad - rest - 1);
  for (int i = 0; i < 4; i++) { /* the length in bits, big-endian */
    tail[pad - 1 - i] = (u8)((len << 3) >> (8 * i));
    tail[pad - 5 - i] = (u8)(i ? 0 : len >> 29);
  }
  sha_block(st, tail);
  if (pad == 128)
    sha_block(st, tail + 64);
  for (int i = 0; i < 32; i++)
    out[i] = (u8)(st[i / 4] >> (24 - 8 * (i % 4)));
}
