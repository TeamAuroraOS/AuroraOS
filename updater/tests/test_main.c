/* Runs updater/main.c once on the PC (run.py sets the scene). The HOME return
 * descriptor and the OS snapshot sit at their console addresses, mapped here,
 * and what the updater left there is written to UT_REPORT when it exits. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define DESC_ADDR     0x25008000u
#define SNAPSHOT_ADDR 0x26000000u
#define SNAPSHOT_MAX  0x00100000u

int updater_main(void);

static void map_at(uint32_t addr, uint32_t size) {
  void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (p != (void *)(uintptr_t)addr) {
    printf("cannot map 0x%08X\n", addr);
    exit(2);
  }
  memset(p, 0, size);
}

static void report(void) {
  volatile uint32_t *desc = (volatile uint32_t *)(uintptr_t)DESC_ADDR;
  const char *path = getenv("UT_REPORT");
  FILE *f = path ? fopen(path, "wb") : NULL;
  if (!f)
    return;
  uint32_t size = desc[1] <= SNAPSHOT_MAX ? desc[1] : 0;
  fwrite(&size, 4, 1, f);
  fwrite((const void *)(uintptr_t)SNAPSHOT_ADDR, 1, size, f);
  fclose(f);
}

int main(void) {
  map_at(DESC_ADDR & ~0xFFFFu, 0x10000u);
  map_at(SNAPSHOT_ADDR, SNAPSHOT_MAX);
  if (!getenv("UT_NO_RETURN"))
    *(volatile uint32_t *)(uintptr_t)DESC_ADDR = 0x52544E31u; /* "RTN1" */
  atexit(report);
  updater_main();
  return 3; /* the updater always leaves through exit() */
}
