#include "GPIO_3DS.h"

#define GPIO_MEM 0x10147000u

int GPIO_write(const char *c) {
  if (c == 0)
    return -1;
  *(volatile unsigned char *)GPIO_MEM = (unsigned char)*c;
  return 0;
}
