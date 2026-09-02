#include "container.h"

static int magic_is_valid(const char m[4]) {
  if (m[0] == 'A' && m[1] == 'O' && m[2] == 'S' && m[3] == '1')
    return 1;
  if (m[0] == 'A' && m[1] == 'U' && m[2] == 'R' && m[3] == '1')
    return 1;
  return 0;
}

aurora_status_t aurora_parse_header(FIL *fp, aos_header_t *hdr) {
  UINT br = 0;
  FRESULT fr = f_read(fp, hdr, sizeof(*hdr), &br);
  if (fr != FR_OK || br != sizeof(*hdr))
    return AURORA_ERR_READ;
  if (!magic_is_valid(hdr->magic))
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
