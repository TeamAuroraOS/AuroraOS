#include "GPIO_3DS.h"

/* 3DS GPIO register block. */
#define GPIO_MEM 0x10147000u

/* Write the byte pointed to by `c` to the GPIO data register.
 * Returns 0 on success, or -1 if `c` is NULL. Unused scaffolding at present. */
int GPIO_write(const char *c) {
  if (c == 0)
    return -1;
  *(volatile unsigned char *)GPIO_MEM = (unsigned char)*c;
  return 0;
}
