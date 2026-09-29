/* File Explorer operations. A walk keeps one DIR per level and one path buffer
 * per side, so its memory is fixed however large the tree is. Entries are
 * removed while their folder is being read, as FatFs's own delete example
 * does. */
#include "fileops.h"
#include "ff.h"
#include "image.h"
#include <string.h>

/* Copies go through the image viewer's file buffer, which is free while the
 * list is showing. */
#define FO_BUF   ((u8 *)IMAGE_FILE_ADDR)
#define FO_CHUNK (1u << 20)

static char src_path[FO_PATH], dst_path[FO_PATH];
static DIR dirs[FO_DEPTH];
static FILINFO info;
static FIL fin, fout;
static u32 deleted;

static FoResult from_fr(FRESULT r) {
  switch (r) {
    case FR_OK:              return FO_OK;
    case FR_EXIST:           return FO_EXISTS;
    case FR_NO_FILE:
    case FR_NO_PATH:         return FO_GONE;
    case FR_INVALID_NAME:    return FO_BAD_NAME;
    case FR_DENIED:
    case FR_WRITE_PROTECTED: return FO_DENIED;
    default:                 return FO_FAILED;
  }
}

static int set_path(char *dst, const char *src) {
  int n = 0;
  for (; src[n]; n++) {
    if (n >= FO_PATH - 1)
      return -1;
    dst[n] = src[n];
  }
  dst[n] = 0;
  return n;
}

/* "/name" onto a path `len` bytes long. The new length, or -1 with the path
 * left as it was when the result would not fit. */
static int append(char *path, int len, const char *name) {
  int n = len;
  if (n && path[n - 1] != '/')
    path[n++] = '/';
  for (int i = 0; name[i]; i++) {
    if (n >= FO_PATH - 1) {
      path[len] = 0;
      return -1;
    }
    path[n++] = name[i];
  }
  path[n] = 0;
  return n;
}

static const char *last_part(const char *path) {
  const char *p = path;
  for (const char *s = path; *s; s++)
    if (*s == '/' && s[1])
      p = s + 1;
  return p;
}

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int fo_within(const char *path, const char *folder) {
  int n = 0;
  if (!folder[0])
    return 1;
  while (folder[n]) {
    if (fold(path[n]) != fold(folder[n]))
      return 0;
    n++;
  }
  return !path[n] || path[n] == '/' || folder[n - 1] == '/';
}

static FoResult delete_one(FoProgress cb) {
  FoResult res = from_fr(f_unlink(src_path));
  if (res == FO_OK) {
    deleted++;
    if (cb && !cb(last_part(src_path), deleted, 0))
      res = FO_CANCELLED;
  }
  return res;
}

/* Empties the folder at src_path, then removes it. */
static FoResult delete_tree(int len, int depth, FoProgress cb) {
  DIR *d = &dirs[depth];
  FoResult res = from_fr(f_opendir(d, src_path));
  if (res != FO_OK)
    return res;

  for (;;) {
    FRESULT r = f_readdir(d, &info);
    if (r != FR_OK) {
      res = from_fr(r);
      break;
    }
    if (!info.fname[0])
      break;
    int n = append(src_path, len, info.fname);
    if (n < 0) {
      res = FO_TOO_DEEP;
      break;
    }
    if (info.fattrib & AM_DIR)
      res = depth + 1 < FO_DEPTH ? delete_tree(n, depth + 1, cb) : FO_TOO_DEEP;
    else
      res = delete_one(cb);
    src_path[len] = 0;
    if (res != FO_OK)
      break;
  }
  f_closedir(d);
  return res == FO_OK ? delete_one(cb) : res;
}

FoResult fo_delete(const char *path, FoProgress cb) {
  int len = set_path(src_path, path);
  FRESULT r;

  if (len < 0)
    return FO_TOO_DEEP;
  deleted = 0;
  r = f_stat(src_path, &info);
  if (r != FR_OK)
    return from_fr(r);
  return (info.fattrib & AM_DIR) ? delete_tree(len, 0, cb) : delete_one(cb);
}

/* src_path to dst_path, which must not exist yet. A partial copy is removed. */
static FoResult copy_file(FoProgress cb) {
  const char *name = last_part(src_path);
  FoResult res;
  u32 size, done = 0;
  UINT br, bw;

  res = from_fr(f_open(&fin, src_path, FA_READ));
  if (res != FO_OK)
    return res;
  res = from_fr(f_open(&fout, dst_path, FA_WRITE | FA_CREATE_NEW));
  if (res != FO_OK) {
    f_close(&fin);
    return res;
  }
  size = (u32)f_size(&fin);
  if (cb && !cb(name, 0, size))
    res = FO_CANCELLED;

  while (res == FO_OK) {
    FRESULT r = f_read(&fin, FO_BUF, FO_CHUNK, &br);
    if (r != FR_OK) {
      res = from_fr(r);
      break;
    }
    if (!br)
      break;
    r = f_write(&fout, FO_BUF, br, &bw);
    if (r != FR_OK)
      res = from_fr(r);
    else if (bw < br) /* FatFs reports a full card as a short write */
      res = FO_FULL;
    done += br;
    if (res == FO_OK && cb && !cb(name, done, size))
      res = FO_CANCELLED;
  }

  f_close(&fin);
  if (f_close(&fout) != FR_OK && res == FO_OK)
    res = FO_FULL;
  if (res != FO_OK)
    f_unlink(dst_path);
  return res;
}

static FoResult copy_tree(int slen, int dlen, int depth, FoProgress cb) {
  DIR *d = &dirs[depth];
  FoResult res = from_fr(f_mkdir(dst_path));
  if (res == FO_DENIED)
    res = FO_FULL; /* dst was checked free, so this is no room for the entry */
  if (res != FO_OK)
    return res;
  res = from_fr(f_opendir(d, src_path));
  if (res != FO_OK)
    return res;

  for (;;) {
    FRESULT r = f_readdir(d, &info);
    if (r != FR_OK) {
      res = from_fr(r);
      break;
    }
    if (!info.fname[0])
      break;
    int sn = append(src_path, slen, info.fname);
    int dn = sn < 0 ? -1 : append(dst_path, dlen, info.fname);
    if (dn < 0) {
      src_path[slen] = 0;
      res = FO_TOO_DEEP;
      break;
    }
    if (info.fattrib & AM_DIR)
      res = depth + 1 < FO_DEPTH ? copy_tree(sn, dn, depth + 1, cb)
                                 : FO_TOO_DEEP;
    else
      res = copy_file(cb);
    src_path[slen] = 0;
    dst_path[dlen] = 0;
    if (res != FO_OK)
      break;
  }
  f_closedir(d);
  return res;
}

FoResult fo_copy(const char *src, const char *dst, FoProgress cb) {
  int slen = set_path(src_path, src), dlen = set_path(dst_path, dst);
  FoResult res;
  FRESULT r;

  if (slen < 0 || dlen < 0)
    return FO_TOO_DEEP;
  if (f_stat(dst_path, &info) == FR_OK)
    return FO_EXISTS;
  r = f_stat(src_path, &info);
  if (r != FR_OK)
    return from_fr(r);
  if (!(info.fattrib & AM_DIR))
    return copy_file(cb);
  if (fo_within(dst_path, src_path))
    return FO_INTO_SELF;

  res = copy_tree(slen, dlen, 0, cb);
  if (res != FO_OK && res != FO_EXISTS) {
    /* The walk has put both paths back, so the top folder it made is dst. */
    dlen = set_path(src_path, dst_path);
    deleted = 0;
    delete_tree(dlen, 0, 0);
  }
  return res;
}

FoResult fo_move(const char *src, const char *dst) {
  if (fo_within(dst, src) && !fo_within(src, dst))
    return FO_INTO_SELF;
  return from_fr(f_rename(src, dst));
}

FoResult fo_mkdir(const char *path) { return from_fr(f_mkdir(path)); }

int fo_unique(char *path, int is_dir) {
  static char stem[FO_PATH];
  const char *ext = "";
  int len = 0, name = 0, dot = -1;

  if (f_stat(path, &info) != FR_OK)
    return 1;
  while (path[len]) {
    if (path[len] == '/')
      name = len + 1;
    len++;
  }
  if (len >= FO_PATH)
    return 0;
  for (int i = name + 1; i < len && !is_dir; i++)
    if (path[i] == '.')
      dot = i;
  memcpy(stem, path, (u32)len + 1u);
  if (dot >= 0) {
    ext = stem + dot;
    len = dot;
  }

  for (u32 n = 2; n < 1000; n++) {
    char num[8];
    int k = 0, w = len;
    for (u32 v = n; v; v /= 10u)
      num[k++] = (char)('0' + v % 10u);
    if (len + 3 + k + (int)strlen(ext) >= FO_PATH)
      return 0;
    memcpy(path, stem, (u32)len);
    path[w++] = ' ';
    path[w++] = '(';
    while (k)
      path[w++] = num[--k];
    path[w++] = ')';
    for (const char *e = ext; *e; e++)
      path[w++] = *e;
    path[w] = 0;
    if (f_stat(path, &info) != FR_OK)
      return 1;
  }
  return 0;
}

const char *fo_error(FoResult r) {
  switch (r) {
    case FO_OK:        return "Done";
    case FO_CANCELLED: return "Cancelled";
    case FO_EXISTS:    return "Something here already has that name";
    case FO_INTO_SELF: return "A folder cannot go inside itself";
    case FO_TOO_DEEP:  return "The folders go too deep";
    case FO_FULL:      return "The SD card is full";
    case FO_GONE:      return "It is no longer there";
    case FO_BAD_NAME:  return "That name is not allowed";
    case FO_DENIED:    return "It is read-only, or the card is full";
    default:           return "The SD card reported an error";
  }
}
