/* Loads the .bin and .ovl plugins in SD:/Aurora/amenu into resident FCRAM so
 * both the OS sidebar and the ARM11 in-app Tesla-style sidebar can run them. */
#include "amenu.h"
#include "aurora.h"
#include "ff.h"

extern void os_cache_sync(void);

static int is_bin_ext(const char *name) {
  int n = 0;
  while (name[n])
    n++;
  if (n < 5 || name[n - 4] != '.')
    return 0;
  char a = name[n - 3], b = name[n - 2], c = name[n - 1];
  if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
  if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
  if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return (a == 'b' && b == 'i' && c == 'n') ||
         (a == 'o' && b == 'v' && c == 'l');
}

static void copy_str(char *dst, const char *src, unsigned max) {
  unsigned i = 0;
  while (src[i] && i + 1 < max) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void join_path(char *out, const char *dir, const char *file) {
  while (*dir)
    *out++ = *dir++;
  *out++ = '/';
  while (*file)
    *out++ = *file++;
  *out = '\0';
}

static int file_is_amnu(const char *path) {
  static FIL f;
  char magic[4];
  UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  int ok = (f_read(&f, magic, 4, &br) == FR_OK && br == 4 &&
            magic[0] == 'A' && magic[1] == 'M' &&
            magic[2] == 'N' && magic[3] == 'U');
  f_close(&f);
  return ok;
}

/* A plugin of the same name already there is kept as <name>.old, which the
 * loader skips, rather than deleted. */
void amenu_migrate_file_mounted(const char *src_path, const char *fname) {
  char dst_path[160], old_path[168];
  char *p = old_path;
  f_mkdir("Aurora");
  f_mkdir(AMENU_DIR);
  join_path(dst_path, AMENU_DIR, fname);
  for (const char *q = dst_path; *q;)
    *p++ = *q++;
  *p++ = '.';
  *p++ = 'o';
  *p++ = 'l';
  *p++ = 'd';
  *p = 0;
  f_unlink(old_path);
  f_rename(dst_path, old_path);
  f_rename(src_path, dst_path);
}

/* Quickly check optional /Aurora/App and /Aurora/App/C folders (instant no-op
 * when those folders do not exist). */
static void migrate_alt_dir_mounted(const char *src_dir) {
  static DIR dir;
  static FILINFO info;
  char src_path[160];

  if (f_opendir(&dir, src_dir) != FR_OK)
    return;
  while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
    if ((info.fattrib & AM_DIR) || !is_bin_ext(info.fname))
      continue;
    join_path(src_path, src_dir, info.fname);
    if (file_is_amnu(src_path))
      amenu_migrate_file_mounted(src_path, info.fname);
  }
  f_closedir(&dir);
}

static int load_one_plugin(const char *filename, uint32_t slot_idx,
                           AmenuSlot *slot) {
  static FIL f;
  char path[160];
  AmenuFileHeader hdr;
  UINT br = 0;
  uint32_t slot_addr = AMENU_POOL_ADDR + slot_idx * AMENU_SLOT_SIZE;

  join_path(path, AMENU_DIR, filename);
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;

  if (f_read(&f, &hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr)) {
    f_close(&f);
    return 0;
  }

  if (hdr.magic[0] != 'A' || hdr.magic[1] != 'M' ||
      hdr.magic[2] != 'N' || hdr.magic[3] != 'U' ||
      hdr.abi_version != AMENU_ABI_VERSION ||
      hdr.payload_size == 0 ||
      hdr.entry_offset >= hdr.payload_size ||
      hdr.payload_size > AMENU_SLOT_SIZE ||
      hdr.bss_size > AMENU_SLOT_SIZE - hdr.payload_size) {
    f_close(&f);
    return 0;
  }

  if (f_read(&f, (void *)slot_addr, hdr.payload_size, &br) != FR_OK ||
      br != hdr.payload_size) {
    f_close(&f);
    return 0;
  }

  volatile uint8_t *bss = (volatile uint8_t *)(slot_addr + hdr.payload_size);
  for (uint32_t i = 0; i < hdr.bss_size; i++)
    bss[i] = 0;

  if (hdr.reloc_count > 0) {
    if (f_lseek(&f, hdr.reloc_offset) != FR_OK) {
      f_close(&f);
      return 0;
    }
    int32_t delta = (int32_t)(slot_addr - hdr.base_addr);
    for (uint32_t i = 0; i < hdr.reloc_count; i++) {
      uint32_t off = 0;
      if (f_read(&f, &off, sizeof(off), &br) != FR_OK || br != sizeof(off)) {
        f_close(&f);
        return 0;
      }
      if (off + 4u <= hdr.payload_size && (off & 3u) == 0u) {
        volatile uint32_t *word = (volatile uint32_t *)(slot_addr + off);
        *word = (uint32_t)((int32_t)(*word) + delta);
      }
    }
  }

  f_close(&f);

  slot->loaded = 1;
  slot->base_addr = slot_addr;
  slot->entry_addr = slot_addr + hdr.entry_offset;
  slot->hud_enabled = 0;
  slot->initialized = 0;
  slot->plugin_ptr = 0;
  copy_str(slot->filename, filename, sizeof(slot->filename));
  return 1;
}

int amenu_load_mounted(void) {
  static DIR dir;
  static FILINFO info;
  AmenuTable *tbl = amenu_table();
  uint32_t gen = (tbl->magic == AMENU_TABLE_MAGIC) ? tbl->generation + 1u : 1u;
  uint32_t count = 0;

  tbl->magic = 0;
  tbl->version = AMENU_ABI_VERSION;
  tbl->count = 0;
  tbl->hud_mask = 0;
  for (uint32_t i = 0; i < AMENU_MAX_PLUGINS; i++) {
    tbl->slots[i].loaded = 0;
    tbl->slots[i].initialized = 0;
    tbl->slots[i].plugin_ptr = 0;
    tbl->slots[i].hud_enabled = 0;
    tbl->slots[i].filename[0] = '\0';
  }
  /* The core must see the table gone before any slot is overwritten. */
  os_cache_sync();

  f_mkdir("Aurora");
  f_mkdir(AMENU_DIR);
  migrate_alt_dir_mounted("Aurora/App");
  migrate_alt_dir_mounted("Aurora/App/C");

  if (f_opendir(&dir, AMENU_DIR) == FR_OK) {
    while (count < AMENU_MAX_PLUGINS &&
           f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
      if ((info.fattrib & AM_DIR) || !is_bin_ext(info.fname))
        continue;
      if (load_one_plugin(info.fname, count, &tbl->slots[count]))
        count++;
    }
    f_closedir(&dir);
  }

  tbl->count = count;
  tbl->generation = gen;
  tbl->magic = AMENU_TABLE_MAGIC;
  os_cache_sync();
  return (int)count;
}

int amenu_load_plugins(void) {
  static FATFS fs;
  int count = 0;
  if (f_mount(&fs, "", 1) == FR_OK) {
    count = amenu_load_mounted();
    f_mount(NULL, "", 0);
  }
  return count;
}
