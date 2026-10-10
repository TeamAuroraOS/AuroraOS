#include "container.h"

int aurora_magic_kind(const char *m) {
  if (m[0] != 'A')
    return 0;
  if (m[1] == 'O' && m[2] == 'S' && m[3] == '1')
    return 'A';
  if (m[1] == 'U' && m[2] == 'R' && m[3] == '1')
    return 'U';
  if (m[1] == 'U' && m[2] == 'R' && m[3] == 'C')
    return 'C';
  return 0;
}

aurora_status_t aurora_parse_header(FIL *fp, aos_header_t *hdr) {
  UINT br = 0;
  FRESULT fr = f_read(fp, hdr, sizeof(*hdr), &br);
  if (fr != FR_OK || br != sizeof(*hdr))
    return AURORA_ERR_READ;
  if (!aurora_magic_kind(hdr->magic))
    return AURORA_ERR_MAGIC;
  return AURORA_OK;
}

aurora_status_t aurora_load_arm9(FIL *fp, const aos_header_t *hdr, void *dst) {
  UINT br = 0;
  FRESULT fr = f_lseek(fp, hdr->arm9_offset);
  if (fr == FR_OK)
    fr = f_read(fp, dst, hdr->arm9_size, &br);
  if (fr != FR_OK || br != hdr->arm9_size)
    return AURORA_ERR_PAYLOAD;
  return AURORA_OK;
}

int aurora_c_data_dir(const char *path, char *out, unsigned max) {
  static const char dir[] = AURORA_C_APPS_DIR "/";
  const char *base = path, *end;
  unsigned n = 0;

  for (const char *s = path; *s; s++)
    if (*s == '/' || *s == ':')
      base = s + 1;
  if (!*base)
    return 0;
  end = base;
  while (*end)
    end++;
  /* the last dot starts the extension, unless it starts the name */
  for (const char *s = end - 1; s > base; s--)
    if (*s == '.') {
      end = s;
      break;
    }
  if (sizeof(dir) + (unsigned)(end - base) > max)
    return 0;
  for (const char *s = dir; *s; s++)
    out[n++] = *s;
  for (const char *s = base; s < end; s++)
    out[n++] = *s;
  out[n] = 0;
  return 1;
}

int aurora_c_data_make(const char *path) {
  static char dir[sizeof(AURORA_C_APPS_DIR) + FF_LFN_BUF + 1];
  FRESULT r;

  if (!aurora_c_data_dir(path, dir, sizeof(dir)))
    return 0;
  f_mkdir("Aurora");
  f_mkdir("Aurora/Apps");
  f_mkdir(AURORA_C_APPS_DIR);
  r = f_mkdir(dir);
  return r == FR_OK || r == FR_EXIST;
}
