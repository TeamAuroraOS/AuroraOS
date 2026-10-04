#ifndef AURORA_AES_H
#define AURORA_AES_H

#include "aurora.h"

/* The ARM9's AES engine, driven a word at a time through its FIFOs (no DMA,
 * so the data cache needs no care). Keyslots above 3 only: the DSi slots are
 * not used. A counter is 16 bytes, most significant first. */

/* A keyslot's keyY. With its keyX already there (the bootrom sets the NAND
 * and NCCH ones), the engine makes the normal key from the two. */
void aes_keyy(int slot, const u8 *keyy);

/* `len` bytes at `buf` (a multiple of 16, word aligned) en- or decrypted in
 * place in CTR mode with `slot`; `ctr` is advanced past them. */
void aes_ctr(int slot, u8 *ctr, void *buf, u32 len);

void aes_ctr_add(u8 *ctr, u32 blocks);

#endif
