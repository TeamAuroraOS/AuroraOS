#ifndef AURORA_NAND_H
#define AURORA_NAND_H

#include "aurora.h"

/* The system NAND, read only. nand_open() brings the eMMC up, finds CTRNAND
 * in the NCSD partition table, sets its keys and checks that it decrypts; it
 * is then FatFs drive "1:". Nothing here can write the NAND. */

enum {
  NAND_OK = 0,
  NAND_ERR_EMMC,  /* the eMMC did not answer or a read failed */
  NAND_ERR_NCSD,  /* no partition table, or no CTRNAND in it */
  NAND_ERR_KEYS,  /* CTRNAND did not decrypt to a partition table */
};

int nand_open(void);

/* Takes drive 1 away again; the eMMC is left as it is. */
void nand_close(void);

const char *nand_error(int err);

void sha256(const void *data, u32 len, u8 out[32]);

/* The same over data that comes in pieces, as aShop's downloads do. */
typedef struct {
  u32 st[8];
  u8 buf[64];
  u32 n, len;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, u32 len);
void sha256_final(Sha256 *s, u8 out[32]);

#endif
