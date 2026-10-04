/* The ARM9 AES engine at 0x10009000. The register sequences follow Luma3DS's
 * crypto.c (GPL-3.0): keys and counters are written big-endian in normal word
 * order, and the counter registers take the last word first. */

#include "aes.h"
#include <string.h>

#define REG_AESCNT      (*(volatile u32 *)0x10009000)
#define REG_AESBLKCNT   (*(volatile u32 *)0x10009004)
#define REG_AESWRFIFO   (*(volatile u32 *)0x10009008)
#define REG_AESRDFIFO   (*(volatile u32 *)0x1000900C)
#define REG_AESKEYSEL   (*(volatile u8 *)0x10009010)
#define REG_AESKEYCNT   (*(volatile u8 *)0x10009011)
#define REG_AESCTR      ((volatile u32 *)0x10009020)
#define REG_AESKEYYFIFO (*(volatile u32 *)0x10009108)

#define CNT_START     0x80000000u
#define CNT_KEY_APPLY 0x04000000u /* loads the slot in AES_KEYSEL */
#define CNT_IN_ORDER  0x02000000u
#define CNT_OUT_ORDER 0x01000000u
#define CNT_IN_BE     0x00800000u
#define CNT_OUT_BE    0x00400000u
#define CNT_FLUSH_RD  0x00000800u
#define CNT_FLUSH_WR  0x00000400u
#define CNT_MODE_CTR  (2u << 27)
#define KEYCNT_WRITE  0x80u

/* The key and counter writes are read in the input byte and word order. */
static void input_be_normal(void) {
  REG_AESCNT = (REG_AESCNT & ~(CNT_IN_BE | CNT_IN_ORDER)) | CNT_IN_BE |
               CNT_IN_ORDER;
}

void aes_keyy(int slot, const u8 *keyy) {
  u32 w[4];
  memcpy(w, keyy, 16);
  input_be_normal();
  REG_AESKEYCNT = (u8)((REG_AESKEYCNT & 0xC0u) | (u32)slot | KEYCNT_WRITE);
  for (int i = 0; i < 4; i++)
    REG_AESKEYYFIFO = w[i];
}

void aes_ctr_add(u8 *ctr, u32 blocks) {
  u32 carry = blocks;
  for (int i = 15; i >= 0 && carry; i--) {
    u32 s = ctr[i] + (carry & 0xFFu);
    ctr[i] = (u8)s;
    carry = (carry >> 8) + (s >> 8);
  }
}

void aes_ctr(int slot, u8 *ctr, void *buf, u32 len) {
  u32 *p = (u32 *)buf;
  u32 blocks = len / 16u;

  REG_AESKEYSEL = (u8)slot;
  REG_AESCNT = REG_AESCNT | CNT_KEY_APPLY;
  while (blocks) {
    u32 n = blocks > 0xFFFFu ? 0xFFFFu : blocks, wr = n, rd = n, w[4];
    const u32 *in = p;
    u32 *out = p;

    REG_AESCNT = CNT_MODE_CTR | CNT_IN_ORDER | CNT_OUT_ORDER | CNT_IN_BE |
                 CNT_OUT_BE | CNT_FLUSH_RD | CNT_FLUSH_WR;
    memcpy(w, ctr, 16);
    input_be_normal();
    REG_AESCTR[0] = w[3];
    REG_AESCTR[1] = w[2];
    REG_AESCTR[2] = w[1];
    REG_AESCTR[3] = w[0];
    REG_AESBLKCNT = n << 16;
    REG_AESCNT |= CNT_START;

    /* A block in once the input FIFO has room for it, a block out once the
     * output FIFO holds one; in place, the reads never pass the writes. */
    while (rd) {
      if (wr && (REG_AESCNT & 0x1Fu) <= 0xCu) {
        REG_AESWRFIFO = *in++;
        REG_AESWRFIFO = *in++;
        REG_AESWRFIFO = *in++;
        REG_AESWRFIFO = *in++;
        wr--;
      }
      if (((REG_AESCNT >> 5) & 0x1Fu) >= 4u) {
        *out++ = REG_AESRDFIFO;
        *out++ = REG_AESRDFIFO;
        *out++ = REG_AESRDFIFO;
        *out++ = REG_AESRDFIFO;
        rd--;
      }
    }
    aes_ctr_add(ctr, n);
    p += n * 4u;
    blocks -= n;
  }
}
