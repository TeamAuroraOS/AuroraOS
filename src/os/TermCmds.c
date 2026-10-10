/* The terminal's commands. Each answers as its Linux namesake does, as far as
 * the console allows; info and running apps are Aurora's own. */

#include "term.h"
#include "audio.h"
#include "container.h"
#include "files.h"
#include "gpu.h"
#include "image.h"
#include "loader.h"
#include "model.h"
#include "power.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include "wifi.h"
#include <string.h>

#define APPS_DIR   "0:/Aurora/Apps"
#define C_APPS_DIR "0:/" AURORA_C_APPS_DIR
#define MUSIC_DIR  "0:/Aurora/Music"

static char *t_num(char *p, u32 v) {
  char tmp[12];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n--)
    *p++ = tmp[n];
  *p = 0;
  return p;
}

static char *t_two(char *p, u32 v) {
  *p++ = (char)('0' + v / 10u % 10u);
  *p++ = (char)('0' + v % 10u);
  *p = 0;
  return p;
}

static char *t_hex(char *p, u32 v) {
  *p++ = '0';
  *p++ = 'x';
  for (int s = 28; s >= 0; s -= 4)
    *p++ = "0123456789ABCDEF"[(v >> s) & 15u];
  *p = 0;
  return p;
}

/* a.b.c.d */
static char *t_ip(char *p, u32 ip) {
  for (int i = 3; i >= 0; i--) {
    p = t_num(p, (ip >> (i * 8)) & 0xFFu);
    if (i)
      *p++ = '.';
  }
  *p = 0;
  return p;
}

/* As ls -h: "934", "1.2K", "34M". */
static char *t_hsize(char *p, u32 b) {
  u32 div = 1024u, whole, tenth;
  int u = 0;
  if (b < 1024u)
    return t_num(p, b);
  while (u < 2 && b / div >= 1024u) {
    div <<= 10;
    u++;
  }
  whole = b / div;
  tenth = (b % div) / (div / 10u);
  p = t_num(p, whole);
  if (whole < 10u) {
    *p++ = '.';
    *p++ = (char)('0' + (tenth > 9u ? 9u : tenth));
  }
  *p++ = "KMG"[u];
  *p = 0;
  return p;
}

/* 2^21 sectors to a GB, so a shift rather than a 64-bit divide. */
static char *t_gb(char *p, unsigned long long sectors) {
  u32 tenths = (u32)((sectors * 10ull + (1ull << 20)) >> 21);
  p = t_num(p, tenths / 10u);
  *p++ = '.';
  *p++ = (char)('0' + tenths % 10u);
  *p = 0;
  return p;
}

/* A FAT timestamp as "2026-09-30 14:05". */
static void t_stamp(char *p, u16 d, u16 t) {
  p = t_num(p, 1980u + (d >> 9));
  *p++ = '-';
  p = t_two(p, (d >> 5) & 15u);
  *p++ = '-';
  p = t_two(p, d & 31u);
  *p++ = ' ';
  p = t_two(p, t >> 11);
  *p++ = ':';
  t_two(p, (t >> 5) & 63u);
}

static u32 rd16le(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32le(const u8 *p) { return rd16le(p) | (rd16le(p + 2) << 16); }
static u32 rd32be(const u8 *p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static const char *t_base(const char *path) {
  const char *b = path;
  for (const char *s = path; *s; s++)
    if (*s == '/' && s[1])
      b = s + 1;
  return b;
}

static int kind_color(int kind) {
  switch (kind) {
    case FKIND_DIR:    return TC_DIR;
    case FKIND_AURORA: return TC_EXEC;
    case FKIND_IMAGE:  return TC_IMAGE;
    case FKIND_AUDIO:  return TC_AUDIO;
    default:           return TC_TEXT;
  }
}

/* Leading "-abc" flags, one bit each in the order of `allowed`. Returns the
 * index of the first operand, or -1 after reporting a flag that is not
 * allowed. "--" ends the flags. */
static int t_flags(const char *cmd, int argc, char **argv, const char *allowed,
                   u32 *on) {
  int i;
  *on = 0;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] != '-' || !a[1])
      break;
    if (a[1] == '-') {
      if (!a[2])
        return i + 1;
      t_err(cmd, "unrecognized option", a, 0);
      return -1;
    }
    for (const char *p = a + 1; *p; p++) {
      const char *f = strchr(allowed, *p);
      if (!f) {
        char bad[2] = {*p, 0};
        t_err(cmd, "invalid option --", bad, 0);
        return -1;
      }
      *on |= 1u << (f - allowed);
    }
  }
  return i;
}

static int has(u32 on, const char *allowed, char c) {
  const char *f = strchr(allowed, c);
  return f && ((on >> (f - allowed)) & 1u);
}

/* An error line built in parts, for the messages t_err cannot shape. */
static char m_buf[2 * T_PATH];
static char *m_at;

static void m_add(const char *s) {
  while (*s && m_at < m_buf + sizeof(m_buf) - 1)
    *m_at++ = *s++;
  *m_at = 0;
}

static void m_start(const char *cmd) {
  m_at = m_buf;
  m_add(cmd);
  m_add(": ");
}

static void m_q(const char *s) {
  m_add("'");
  m_add(s);
  m_add("'");
}

static void m_end(void) { t_line(m_buf, TC_ERR); }

/* After a folder was moved or removed out from under the shell. */
static void t_fix_cwd(void) {
  static FILINFO fno;
  while (!t_is_root(t_cwd) &&
         (t_stat(t_cwd, &fno) != FR_OK || !(fno.fattrib & AM_DIR))) {
    int n = (int)strlen(t_cwd);
    while (n > 3 && t_cwd[n - 1] != '/')
      n--;
    if (n > 3)
      n--;
    t_cwd[n] = 0;
  }
}

/* Each part of `path` as it is spelled on the card, so "cd aurora" shows
 * "/Aurora". */
static void t_canon(char *path) {
  static char out[T_PATH];
  static FILINFO fno;
  const char *s = path + 3;
  int n = 3;

  memcpy(out, path, 3);
  out[3] = 0;
  while (*s) {
    const char *e = s;
    int at, len;
    while (*e && *e != '/')
      e++;
    len = (int)(e - s);
    if (n > 3)
      out[n++] = '/';
    at = n;
    memcpy(out + n, s, (u32)len);
    n += len;
    out[n] = 0;
    if (f_stat(out, &fno) == FR_OK) {
      int real = (int)strlen(fno.fname);
      if (at + real < T_PATH) {
        memcpy(out + at, fno.fname, (u32)real + 1u);
        n = at + real;
      }
    }
    s = *e ? e + 1 : e;
  }
  memcpy(path, out, (u32)n + 1u);
}

static char t_job[96];

static void job_set(const char *verb, const char *name, int pct) {
  char *p = t_cpy(t_job, verb);
  int n = 0;
  while (name[n] && n < 40)
    *p++ = name[n++];
  if (name[n])
    p = t_cpy(p, "...");
  if (pct >= 0) {
    *p++ = ' ';
    p = t_num(p, (u32)pct);
    *p++ = '%';
  }
  *p = 0;
}

static int rm_progress(const char *name, u32 done, u32 total) {
  (void)done;
  (void)total;
  job_set("Removing ", name, -1);
  return t_busy(t_job);
}

static int cp_progress(const char *name, u32 done, u32 total) {
  u32 pct = total >= 100u ? done / (total / 100u) : 100u;
  job_set("Copying ", name, (int)(pct > 100u ? 100u : pct));
  return t_busy(t_job);
}

static void t_power(int reboot) {
  t_line(reboot ? "Rebooting..." : "Powering off...", TC_TEXT);
  t_flush();
  t_unmount();
  if (reboot)
    power_reboot();
  else
    power_shutdown();
}

static void ls_long(const char *shown, u32 size, u16 date, u16 time, u8 attr,
                    int kind) {
  char buf[24];
  int dir = (attr & AM_DIR) != 0;
  buf[0] = dir ? 'd' : '-';
  buf[1] = 'r';
  buf[2] = (attr & AM_RDO) ? '-' : 'w';
  buf[3] = (dir || kind == FKIND_AURORA) ? 'x' : '-';
  buf[4] = 0;
  t_out(buf, TC_DIM);
  if (dir)
    t_cpy(buf, "-");
  else
    t_hsize(buf, size);
  for (int w = t_width(buf); w < 6; w++)
    t_outc(' ', TC_TEXT);
  t_out(buf, TC_TEXT);
  t_outc(' ', TC_TEXT);
  t_stamp(buf, date, time);
  t_out(buf, TC_DIM);
  t_outc(' ', TC_TEXT);
  t_line(shown, kind_color(kind));
}

static void cmd_ls(int argc, char **argv) {
  static const char fl[] = "la1h";
  static char path[T_PATH];
  static FILINFO fno;
  static const char *names[T_ENTS];
  static u8 colors[T_ENTS];
  u32 on;
  int i = t_flags("ls", argc, argv, fl, &on), ops, lng, one;

  if (i < 0 || !t_need_sd("ls"))
    return;
  ops = argc - i;
  lng = has(on, fl, 'l');
  one = has(on, fl, '1');

  for (int a = 0; a < (ops ? ops : 1); a++) {
    const char *arg = ops ? argv[i + a] : ".";
    FRESULT r;
    int n;

    if (!t_resolve(arg, path)) {
      t_err("ls", "cannot access", arg, "File name too long");
      continue;
    }
    r = t_stat(path, &fno);
    if (r != FR_OK) {
      t_err("ls", "cannot access", arg, t_frerr(r));
      continue;
    }
    if (!(fno.fattrib & AM_DIR)) {
      int kind = files_kind(fno.fname, 0);
      if (kind == FKIND_BIN && t_container(path))
        kind = FKIND_AURORA;
      if (lng)
        ls_long(arg, (u32)fno.fsize, fno.fdate, fno.ftime, fno.fattrib, kind);
      else
        t_line(arg, kind_color(kind));
      continue;
    }

    if (ops > 1) {
      if (a)
        t_outc('\n', TC_TEXT);
      t_out(arg, TC_TEXT);
      t_line(":", TC_TEXT);
    }
    n = t_readdir(path, has(on, fl, 'a'), 1, &r);
    if (n < 0) {
      t_err("ls", "cannot open directory", arg, t_frerr(r));
      continue;
    }
    if (lng || one) {
      for (int k = 0; k < n; k++) {
        const TEnt *e = &t_ents[k];
        if (lng)
          ls_long(e->name, e->size, e->date, e->time, e->attr, e->kind);
        else
          t_line(e->name, kind_color(e->kind));
      }
    } else {
      for (int k = 0; k < n; k++) {
        names[k] = t_ents[k].name;
        colors[k] = (u8)kind_color(t_ents[k].kind);
      }
      t_columns(names, colors, n);
    }
    if (t_ents_cut) {
      t_out("ls: only the first ", TC_WARN);
      t_u32((u32)n, TC_WARN);
      t_line(" entries fit", TC_WARN);
    }
  }
}

static void cmd_cd(int argc, char **argv) {
  static char next[T_PATH];
  static FILINFO fno;
  const char *arg = argc > 1 ? argv[1] : "~";
  int back = t_streq(arg, "-");
  FRESULT r;

  if (argc > 2) {
    t_err("cd", 0, 0, "too many arguments");
    return;
  }
  if (back) {
    if (!t_oldcwd[0]) {
      t_err("cd", 0, 0, "OLDPWD not set");
      return;
    }
    arg = t_show(t_oldcwd);
  }
  if (!t_need_sd("cd"))
    return;
  if (!t_resolve(arg, next)) {
    t_err("cd", 0, arg, "File name too long");
    return;
  }
  r = t_stat(next, &fno);
  if (r != FR_OK) {
    t_err("cd", 0, arg, t_frerr(r));
    return;
  }
  if (!(fno.fattrib & AM_DIR)) {
    t_err("cd", 0, arg, "Not a directory");
    return;
  }
  t_canon(next);
  t_cpy(t_oldcwd, t_cwd);
  t_cpy(t_cwd, next);
  if (back)
    t_line(t_show(t_cwd), TC_TEXT);
}

static void cmd_pwd(int argc, char **argv) {
  (void)argc;
  (void)argv;
  t_line(t_show(t_cwd), TC_TEXT);
}

static FRESULT mkdir_p(char *p) {
  static FILINFO fno;
  int n = (int)strlen(p);
  for (int i = 4; i <= n; i++) {
    char c = p[i];
    FRESULT r;
    if (i < n && c != '/')
      continue;
    p[i] = 0;
    r = f_mkdir(p);
    if (r == FR_EXIST && f_stat(p, &fno) == FR_OK && (fno.fattrib & AM_DIR))
      r = FR_OK;
    p[i] = c;
    if (r != FR_OK)
      return r;
  }
  return FR_OK;
}

static void cmd_mkdir(int argc, char **argv) {
  static const char fl[] = "p";
  static char path[T_PATH];
  u32 on;
  int i = t_flags("mkdir", argc, argv, fl, &on);

  if (i < 0)
    return;
  if (i >= argc) {
    t_err("mkdir", 0, 0, "missing operand");
    return;
  }
  if (!t_need_sd("mkdir"))
    return;
  for (; i < argc; i++) {
    FRESULT r;
    if (!t_resolve(argv[i], path)) {
      t_err("mkdir", "cannot create directory", argv[i], "File name too long");
      continue;
    }
    if (t_is_root(path))
      r = on ? FR_OK : FR_EXIST;
    else
      r = on ? mkdir_p(path) : f_mkdir(path);
    if (r != FR_OK)
      t_err("mkdir", "cannot create directory", argv[i], t_frerr(r));
  }
}

static void cmd_rm(int argc, char **argv) {
  static const char fl[] = "rRf";
  static char path[T_PATH];
  static FILINFO fno;
  u32 on;
  int i = t_flags("rm", argc, argv, fl, &on), rec, force;

  if (i < 0)
    return;
  rec = has(on, fl, 'r') || has(on, fl, 'R');
  force = has(on, fl, 'f');
  if (i >= argc) {
    if (!force)
      t_err("rm", 0, 0, "missing operand");
    return;
  }
  if (!t_need_sd("rm"))
    return;

  for (; i < argc; i++) {
    const char *b = t_base(argv[i]);
    FRESULT r;
    FoResult res;
    if (t_streq(b, ".") || t_streq(b, "..")) {
      t_err("rm", "refusing to remove '.' or '..' directory: skipping",
            argv[i], 0);
      continue;
    }
    if (!t_resolve(argv[i], path)) {
      t_err("rm", "cannot remove", argv[i], "File name too long");
      continue;
    }
    if (t_is_root(path)) {
      t_err("rm", 0, 0, "it is dangerous to operate recursively on '/'");
      continue;
    }
    r = t_stat(path, &fno);
    if (r != FR_OK) {
      if (!force || (r != FR_NO_FILE && r != FR_NO_PATH))
        t_err("rm", "cannot remove", argv[i], t_frerr(r));
      continue;
    }
    if ((fno.fattrib & AM_DIR) && !rec) {
      t_err("rm", "cannot remove", argv[i], "Is a directory");
      continue;
    }
    res = fo_delete(path, rm_progress);
    if (res == FO_CANCELLED) {
      t_err("rm", 0, 0, "interrupted");
      break;
    }
    if (res != FO_OK)
      t_err("rm", "cannot remove", argv[i], t_foerr(res));
  }
  t_fix_cwd();
}

/* mv and cp: SRC DEST, or SRC... DIR. A file in the way is replaced, as on
 * Linux, unless `noclob`. */
static void t_transfer(const char *cmd, int argc, char **argv, int i,
                       int copy, int rec, int noclob) {
  static char dst[T_PATH], src[T_PATH], to[T_PATH], moved[T_PATH];
  static FILINFO dfi, sfi, tfi;
  const char *darg;
  int ddir;

  if (i >= argc) {
    t_err(cmd, 0, 0, "missing file operand");
    return;
  }
  if (i == argc - 1) {
    t_err(cmd, "missing destination file operand after", argv[i], 0);
    return;
  }
  if (!t_need_sd(cmd))
    return;
  darg = argv[argc - 1];
  if (!t_resolve(darg, dst)) {
    t_err(cmd, 0, darg, "File name too long");
    return;
  }
  ddir = t_stat(dst, &dfi) == FR_OK && (dfi.fattrib & AM_DIR);
  if (argc - i > 2 && !ddir) {
    t_err(cmd, "target", darg, "Not a directory");
    return;
  }

  for (; i < argc - 1; i++) {
    const char *sarg = argv[i];
    FRESULT r;
    FoResult res;
    int sdir, exists;

    if (!t_resolve(sarg, src)) {
      t_err(cmd, 0, sarg, "File name too long");
      continue;
    }
    r = t_stat(src, &sfi);
    if (r != FR_OK) {
      t_err(cmd, "cannot stat", sarg, t_frerr(r));
      continue;
    }
    if (t_is_root(src)) {
      t_err(cmd, copy ? "cannot copy" : "cannot move", sarg,
            "Device or resource busy");
      continue;
    }
    sdir = (sfi.fattrib & AM_DIR) != 0;
    if (copy && sdir && !rec) {
      t_err(cmd, "-r not specified; omitting directory", sarg, 0);
      continue;
    }
    if (ddir) {
      if (!t_join(to, dst, t_base(src))) {
        t_err(cmd, 0, sarg, "File name too long");
        continue;
      }
    } else {
      t_cpy(to, dst);
    }
    if (t_streq(src, to) || (copy && !t_icmp(src, to))) {
      m_start(cmd);
      m_q(sarg);
      m_add(" and ");
      m_q(t_show(to));
      m_add(" are the same file");
      m_end();
      continue;
    }

    /* A rename that only changes the letter case is the same entry. */
    exists = t_icmp(src, to) && t_stat(to, &tfi) == FR_OK;
    if (exists && noclob)
      continue;
    if (exists) {
      int tdir = (tfi.fattrib & AM_DIR) != 0;
      if (tdir || sdir) {
        m_start(cmd);
        if (tdir && !sdir) {
          m_add("cannot overwrite directory ");
          m_q(t_show(to));
          m_add(" with non-directory");
        } else if (!tdir) {
          m_add("cannot overwrite non-directory ");
          m_q(t_show(to));
          m_add(" with directory ");
          m_q(sarg);
        } else {
          m_add(copy ? "cannot copy " : "cannot move ");
          m_q(sarg);
          m_add(" to ");
          m_q(t_show(to));
          m_add(": File exists");
        }
        m_end();
        continue;
      }
      r = f_unlink(to);
      if (r != FR_OK) {
        t_err(cmd, "cannot remove", t_show(to), t_frerr(r));
        continue;
      }
    }

    res = copy ? fo_copy(src, to, cp_progress) : fo_move(src, to);
    if (res == FO_CANCELLED) {
      t_err(cmd, 0, 0, "interrupted");
      break;
    }
    if (res != FO_OK) {
      m_start(cmd);
      if (res == FO_INTO_SELF) {
        m_add(copy ? "cannot copy a directory, " : "cannot move ");
        m_q(sarg);
        m_add(copy ? ", into itself, " : " to a subdirectory of itself, ");
        m_q(t_show(to));
      } else {
        m_add(copy ? "cannot copy " : "cannot move ");
        m_q(sarg);
        m_add(" to ");
        m_q(t_show(to));
        m_add(": ");
        m_add(t_foerr(res));
      }
      m_end();
      continue;
    }
    if (!copy && fo_within(t_cwd, src)) {
      u32 sl = (u32)strlen(src);
      if (strlen(to) + strlen(t_cwd + sl) < T_PATH) {
        t_cpy(t_cpy(moved, to), t_cwd + sl);
        t_cpy(t_cwd, moved);
      }
    }
  }
  if (!copy)
    t_fix_cwd();
}

static void cmd_mv(int argc, char **argv) {
  static const char fl[] = "fn";
  u32 on;
  int i = t_flags("mv", argc, argv, fl, &on);
  if (i >= 0)
    t_transfer("mv", argc, argv, i, 0, 0, has(on, fl, 'n'));
}

static void cmd_cp(int argc, char **argv) {
  static const char fl[] = "rRfn";
  u32 on;
  int i = t_flags("cp", argc, argv, fl, &on);
  if (i >= 0)
    t_transfer("cp", argc, argv, i, 1, has(on, fl, 'r') || has(on, fl, 'R'),
               has(on, fl, 'n'));
}

static void cmd_touch(int argc, char **argv) {
  static char path[T_PATH];
  static FILINFO fno;
  static FIL f;

  if (argc < 2) {
    t_err("touch", 0, 0, "missing file operand");
    return;
  }
  if (!t_need_sd("touch"))
    return;
  for (int i = 1; i < argc; i++) {
    FRESULT r;
    if (!t_resolve(argv[i], path)) {
      t_err("touch", "cannot touch", argv[i], "File name too long");
      continue;
    }
    if (t_stat(path, &fno) == FR_OK)
      continue;
    r = f_open(&f, path, FA_WRITE | FA_CREATE_NEW);
    if (r != FR_OK) {
      t_err("touch", "cannot touch", argv[i], t_frerr(r));
      continue;
    }
    f_close(&f);
  }
}

static void cmd_cat(int argc, char **argv) {
  static char path[T_PATH], buf[2048];
  static FILINFO fno;
  static FIL f;

  if (argc < 2) {
    t_err("cat", 0, 0, "missing file operand");
    return;
  }
  if (!t_need_sd("cat"))
    return;
  for (int i = 1; i < argc; i++) {
    FRESULT r;
    UINT br;
    int stop = 0;
    if (!t_resolve(argv[i], path)) {
      t_err("cat", 0, argv[i], "File name too long");
      continue;
    }
    r = t_stat(path, &fno);
    if (r == FR_OK && (fno.fattrib & AM_DIR)) {
      t_err("cat", 0, argv[i], "Is a directory");
      continue;
    }
    if (r == FR_OK)
      r = f_open(&f, path, FA_READ);
    if (r != FR_OK) {
      t_err("cat", 0, argv[i], t_frerr(r));
      continue;
    }
    while (!stop && f_read(&f, buf, sizeof(buf), &br) == FR_OK && br) {
      for (UINT k = 0; k < br; k++)
        if (buf[k] != '\r')
          t_outc(buf[k], TC_TEXT);
      stop = !t_busy(0);
    }
    f_close(&f);
    if (stop) {
      t_outc('\n', TC_TEXT);
      t_err("cat", 0, 0, "interrupted");
      return;
    }
  }
}

static void info_row(const char *label, const char *value, int color) {
  t_out("  ", TC_TEXT);
  t_pad(label, 10, TC_DIM);
  t_wrap(value, 12, color);
}

static u32 info_read(FIL *f, u32 off, void *buf, u32 len) {
  UINT br = 0;
  if (f_lseek(f, off) != FR_OK || f_read(f, buf, len, &br) != FR_OK)
    return 0;
  return br;
}

/* The frame marker of a JPEG, with its size: 0xC0 and 0xC1 are the baseline
 * kinds the decoder takes. 0 when none turns up. */
static int jpeg_sof(FIL *f, u32 size, u32 *w, u32 *h) {
  u8 b[5];
  u32 pos = 2;
  for (int n = 0; n < 64 && pos + 4 <= size; n++) {
    u8 m;
    if (info_read(f, pos, b, 4) < 4 || b[0] != 0xFF)
      return 0;
    m = b[1];
    if (m == 0xFF) {
      pos++;
      continue;
    }
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {
      pos += 2;
      continue;
    }
    if (m == 0xD9 || m == 0xDA)
      return 0;
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (info_read(f, pos + 4, b, 5) < 5)
        return 0;
      *h = ((u32)b[1] << 8) | b[2];
      *w = ((u32)b[3] << 8) | b[4];
      return m;
    }
    pos += 2u + (((u32)b[2] << 8) | b[3]);
  }
  return 0;
}

static int wav_fmt(FIL *f, u32 size, u32 *fmt, u32 *ch, u32 *rate, u32 *bits,
                   u32 *data) {
  u8 b[16];
  u32 pos = 12;
  int got = 0;
  if (info_read(f, 0, b, 12) < 12 || memcmp(b, "RIFF", 4) ||
      memcmp(b + 8, "WAVE", 4))
    return 0;
  while (pos + 8u <= size && got != 3) {
    u32 len;
    if (info_read(f, pos, b, 8) < 8)
      break;
    len = rd32le(b + 4);
    if (!memcmp(b, "fmt ", 4) && info_read(f, pos + 8u, b, 16) == 16) {
      *fmt = rd16le(b);
      *ch = rd16le(b + 2);
      *rate = rd32le(b + 4);
      *bits = rd16le(b + 14);
      got |= 1;
    } else if (!memcmp(b, "data", 4)) {
      *data = len;
      got |= 2;
    }
    if (len > size - pos)
      break;
    pos += 8u + len + (len & 1u);
  }
  return got == 3;
}

/* "44100 Hz, 16-bit, stereo, 3:25". */
static void audio_line(char *p, u32 rate, u32 bits, u32 ch, u32 bytes) {
  u32 per_sec = rate * ch * (bits / 8u), secs;
  p = t_num(p, rate);
  p = t_cpy(p, " Hz, ");
  p = t_num(p, bits);
  p = t_cpy(p, "-bit, ");
  p = t_cpy(p, ch == 1 ? "mono" : ch == 2 ? "stereo" : "multichannel");
  if (!per_sec)
    return;
  secs = bytes / per_sec;
  p = t_cpy(p, ", ");
  if (secs >= 3600u) {
    p = t_num(p, secs / 3600u);
    *p++ = ':';
    p = t_two(p, secs / 60u % 60u);
  } else {
    p = t_num(p, secs / 60u);
  }
  *p++ = ':';
  t_two(p, secs % 60u);
}

/* Whether `dir` is the folder `path` sits in, ignoring letter case. */
static int in_dir(const char *path, const char *dir) {
  int n = (int)(t_base(path) - path);
  if (n > 3)
    n--; /* the slash before the name, unless it is the root's */
  if (n != (int)strlen(dir))
    return 0;
  for (int i = 0; i < n; i++)
    if (t_fold(path[i]) != t_fold(dir[i]))
      return 0;
  return 1;
}

/* Whether an app's payloads lie within its file, which the loader needs. */
static int app_sane(const aos_header_t *h, u32 size) {
  return h->arm9_size && h->arm9_offset <= size &&
         h->arm9_size <= size - h->arm9_offset &&
         (!h->arm11_size || (h->arm11_offset <= size &&
                             h->arm11_size <= size - h->arm11_offset));
}

/* Kinds and limits as the File Explorer, the viewers and the app loader
 * decide them, so the answer matches what opening the file would do. */
static void info_file(const char *arg, const char *path, const FILINFO *fno) {
  static FIL f;
  static char v[200];
  static u8 hdr[64];
  const char *name = fno->fname, *ext = name;
  u32 size = (u32)fno->fsize, got;
  int kind = files_kind(name, 0), magic, firm;
  char *p;

  for (const char *s = name; *s; s++)
    if (*s == '.')
      ext = s + 1;
  if (ext == name)
    ext = "";

  if (f_open(&f, path, FA_READ) != FR_OK) {
    t_err("info", "cannot read", arg, "Input/output error");
    return;
  }
  memset(hdr, 0, sizeof(hdr));
  got = info_read(&f, 0, hdr, sizeof(hdr));
  magic = got >= 4 ? aurora_magic_kind((const char *)hdr) : 0;
  firm = got >= 4 && !memcmp(hdr, "FIRM", 4);
  if (magic)
    kind = FKIND_AURORA;

  t_line(name, kind_color(kind));
  info_row("Path:", t_show(path), TC_TEXT);

  switch (kind) {
    case FKIND_AURORA:
      p = t_cpy(v, magic == 'U'   ? "Aurora app (AUR1)"
                   : magic == 'C' ? "Aurora app in C (AURC)"
                                  : "AuroraOS image (AOS1)");
      break;
    case FKIND_IMAGE:
      p = t_cpy(v, (ext[0] | 32) == 'p'   ? "PNG image"
                   : (ext[0] | 32) == 'b' ? "BMP image"
                                          : "JPEG image");
      break;
    case FKIND_AUDIO:
      p = t_cpy(v, (ext[0] | 32) == 'w'   ? "WAV audio"
                   : (ext[0] | 32) == 'm' ? "MP3 audio"
                                          : "Aurora audio (AAF)");
      break;
    case FKIND_TEXT:
      p = t_cpy(v, (ext[0] | 32) == 'l' ? "log file" : "text file");
      break;
    case FKIND_BIN:
      p = t_cpy(v, firm ? "FIRM image" : "binary file");
      break;
    default:
      p = t_cpy(v, firm ? "FIRM image" : "file");
      if (!firm && ext[0]) {
        p = t_cpy(p, " (.");
        p = t_cpy(p, ext);
        t_cpy(p, ")");
      }
      break;
  }
  info_row("Type:", v, TC_TEXT);

  p = size < 1024u ? v : t_cpy(t_hsize(v, size), " (");
  p = t_num(p, size);
  p = t_cpy(p, " bytes");
  if (size >= 1024u)
    t_cpy(p, ")");
  info_row("Size:", v, TC_TEXT);
  t_stamp(v, fno->fdate, fno->ftime);
  info_row("Modified:", v, TC_TEXT);
  p = v;
  if (fno->fattrib & AM_RDO)
    p = t_cpy(p, "read-only ");
  if (fno->fattrib & AM_HID)
    p = t_cpy(p, "hidden ");
  if (fno->fattrib & AM_SYS)
    p = t_cpy(p, "system ");
  if (fno->fattrib & AM_ARC)
    p = t_cpy(p, "archive ");
  if (p == v)
    p = t_cpy(p, "none ");
  p[-1] = 0;
  info_row("Flags:", v, TC_TEXT);

  if (kind == FKIND_AURORA) {
    aos_header_t h;
    char sig[8];
    int sane, icon;
    const char *b = t_base(path);
    memcpy(&h, hdr, sizeof(h));
    sane = app_sane(&h, size);
    icon = info_read(&f, h.arm9_offset + AURORA_ICON_MAGIC_OFFSET, sig, 8) ==
               8 &&
           !memcmp(sig, AURORA_ICON_MAGIC, 8);

    p = t_hsize(v, h.arm9_size);
    p = t_cpy(p, " at ");
    p = t_hex(p, h.arm9_load_addr);
    p = t_cpy(p, ", entry ");
    t_hex(p, h.arm9_entry);
    info_row("ARM9:", v, TC_TEXT);
    if (h.arm11_size) {
      p = t_hsize(v, h.arm11_size);
      p = t_cpy(p, " at ");
      t_hex(p, h.arm11_load_addr);
    } else {
      t_cpy(v, "none");
    }
    info_row("ARM11:", v, TC_TEXT);
    info_row("Icon:", icon ? "yes" : "no: the Home Menu draws a plain tile",
             TC_TEXT);
    v[0] = '/';
    if (magic == 'C' && aurora_c_data_dir(path, v + 1, sizeof(v) - 1))
      info_row("Data:", v, TC_TEXT);

    if (!sane) {
      info_row("Execute:", "no: the header points past the end of the file",
               TC_ERR);
    } else {
      p = t_cpy(v, "yes: type ");
      if (in_dir(path, APPS_DIR) || in_dir(path, C_APPS_DIR)) {
        int n = (int)strlen(b);
        if (n > 4 && !t_icmp(b + n - 4, ".bin"))
          n -= 4;
        memcpy(p, b, (u32)n);
        t_cpy(p + n, ", or pick it on the Home Menu");
      } else if (in_dir(path, t_cwd)) {
        t_cpy(t_cpy(p, "./"), b);
      } else {
        t_cpy(p, t_show(path));
      }
      info_row("Execute:", v, TC_EXEC);
    }
    info_row("Open:", "yes: the File Explorer runs it", TC_EXEC);
  } else if (kind == FKIND_IMAGE) {
    u32 w = 0, hh = 0;
    const char *why = 0;
    if (got >= 8 && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N') {
      w = rd32be(hdr + 16);
      hh = rd32be(hdr + 20);
      if (hdr[28])
        why = "no: interlaced PNG is not supported";
    } else if (got >= 34 && hdr[0] == 'B' && hdr[1] == 'M') {
      u32 bpp = rd16le(hdr + 28);
      w = rd32le(hdr + 18);
      hh = rd32le(hdr + 22);
      if ((s32)hh < 0)
        hh = (u32)(-(s32)hh);
      if (rd32le(hdr + 30) || (bpp != 8 && bpp != 24 && bpp != 32))
        why = "no: only uncompressed 8, 24 and 32-bit BMP";
    } else if (got >= 2 && hdr[0] == 0xFF && hdr[1] == 0xD8) {
      int m = jpeg_sof(&f, size, &w, &hh);
      if (m && m != 0xC0 && m != 0xC1)
        why = "no: only baseline JPEG; this one is progressive or lossless";
      else if (!m)
        why = "no: the JPEG has no frame header";
    } else {
      why = "no: the contents are not a BMP, PNG or JPEG";
    }
    if (w && hh) {
      p = t_num(v, w);
      p = t_cpy(p, " x ");
      t_num(p, hh);
      info_row("Image:", v, TC_TEXT);
    }
    if (!why && size > IMAGE_FILE_MAX)
      why = "no: pictures over 6 MB do not load";
    if (!why && (w > IMAGE_MAX_DIM || hh > IMAGE_MAX_DIM ||
                 w * hh > IMAGE_MAX_PIXELS))
      why = "no: more pixels than the viewer holds";
    info_row("Execute:", "no", TC_DIM);
    info_row("Open:", why ? why : "yes: the image viewer in Files shows it",
             why ? TC_WARN : TC_EXEC);
  } else if (kind == FKIND_AUDIO) {
    u32 fmt = 0, ch = 0, rate = 0, bits = 0, data = 0;
    const char *open;
    int col = TC_EXEC;
    if ((ext[0] | 32) == 'w') {
      if (wav_fmt(&f, size, &fmt, &ch, &rate, &bits, &data)) {
        audio_line(v, rate, bits, ch, data);
        info_row("Audio:", v, TC_TEXT);
      }
      open = "yes: the File Explorer plays it";
      if (fmt != 1 || (bits != 8 && bits != 16)) {
        open = "no: only uncompressed 8 and 16-bit PCM plays";
        col = TC_WARN;
      }
    } else if ((ext[0] | 32) == 'm') {
      open = "no: MP3 is not decoded yet; Files offers the hex editor";
      col = TC_WARN;
    } else {
      if (got >= 16 && !memcmp(hdr, "AAF1", 4)) {
        u32 depth = hdr[6];
        audio_line(v, rd32le(hdr + 8), depth, 1, rd32le(hdr + 12) * (depth / 8u));
        info_row("Audio:", v, TC_TEXT);
        open = in_dir(path, MUSIC_DIR)
                   ? "yes: the Music app plays it"
                   : "yes, from /Aurora/Music: the Music app plays it";
      } else {
        open = "no: not an AAF file (no AAF1 header)";
        col = TC_WARN;
      }
    }
    info_row("Execute:", "no", TC_DIM);
    info_row("Open:", open, col);
  } else if (kind == FKIND_TEXT) {
    info_row("Execute:", "no", TC_DIM);
    info_row("Open:", "yes: the text viewer in Files shows it, and cat prints it",
             TC_EXEC);
  } else {
    info_row("Execute:",
             firm ? "no: boot a FIRM from Luma's chainloader (hold START at "
                    "power-on)"
             : kind == FKIND_BIN ? "no: not an Aurora app (no AOS1, AUR1 or "
                                   "AURC header)"
                                 : "no",
             TC_DIM);
    info_row("Open:", "only as hex, in the File Explorer", TC_WARN);
  }
  f_close(&f);
}

static void info_dir(const char *path, const FILINFO *fno) {
  static DIR d;
  static FILINFO e;
  static char v[64];
  u32 dirs = 0, files = 0;
  char *p;

  t_line(t_is_root(path) ? "/" : fno->fname, TC_DIR);
  info_row("Path:", t_show(path), TC_TEXT);
  info_row("Type:", t_is_root(path) ? "folder, the root of the SD card"
                                    : "folder",
           TC_TEXT);
  if (f_opendir(&d, path) == FR_OK) {
    while (f_readdir(&d, &e) == FR_OK && e.fname[0]) {
      if (e.fattrib & AM_DIR)
        dirs++;
      else
        files++;
    }
    f_closedir(&d);
  }
  p = t_num(v, dirs);
  p = t_cpy(p, dirs == 1 ? " folder, " : " folders, ");
  p = t_num(p, files);
  t_cpy(p, files == 1 ? " file" : " files");
  info_row("Holds:", v, TC_TEXT);
  if (!t_is_root(path)) {
    t_stamp(v, fno->fdate, fno->ftime);
    info_row("Modified:", v, TC_TEXT);
  }
  info_row("Execute:", "no", TC_DIM);
  info_row("Open:", "yes: cd into it, or browse it in Files", TC_EXEC);
}

static void cmd_info(int argc, char **argv) {
  static char path[T_PATH];
  static FILINFO fno;

  if (argc < 2) {
    t_err("info", 0, 0, "missing file operand");
    return;
  }
  if (!t_need_sd("info"))
    return;
  for (int i = 1; i < argc; i++) {
    FRESULT r;
    if (i > 1)
      t_outc('\n', TC_TEXT);
    if (!t_resolve(argv[i], path)) {
      t_err("info", "cannot access", argv[i], "File name too long");
      continue;
    }
    r = t_stat(path, &fno);
    if (r != FR_OK)
      t_err("info", "cannot access", argv[i], t_frerr(r));
    else if (fno.fattrib & AM_DIR)
      info_dir(path, &fno);
    else
      info_file(argv[i], path, &fno);
  }
}

static void sh_err(const char *what, const char *msg) {
  t_out("aurora: ", TC_ERR);
  t_out(what, TC_ERR);
  t_out(": ", TC_ERR);
  t_line(msg, TC_ERR);
}

/* The app `name` or `name`.bin in `dir`. */
static int app_in(char *path, const char *dir, const char *name,
                  FILINFO *fno) {
  if (!t_join(path, dir, name))
    return 0;
  if (t_stat(path, fno) != FR_OK || (fno->fattrib & AM_DIR)) {
    if (strlen(path) + 4u >= T_PATH)
      return 0;
    t_cpy(path + strlen(path), ".bin");
    if (t_stat(path, fno) != FR_OK || (fno->fattrib & AM_DIR))
      return 0;
  }
  return t_container(path);
}

int t_exec(int argc, char **argv) {
  static char path[T_PATH];
  static FILINFO fno;
  const char *a = argv[0];
  (void)argc;

  if (!strchr(a, '/')) {
    /* Like $PATH: a bare name runs the app of that name in /Aurora/Apps, or
     * in /Aurora/Apps/C, where C apps go. */
    if (!t_need_sd(0) || (!app_in(path, APPS_DIR, a, &fno) &&
                          !app_in(path, C_APPS_DIR, a, &fno)))
      return 0;
  } else {
    FRESULT r;
    if (!t_need_sd("aurora"))
      return 1;
    if (!t_resolve(a, path)) {
      sh_err(a, "File name too long");
      return 1;
    }
    r = t_stat(path, &fno);
    if (r != FR_OK) {
      sh_err(a, t_frerr(r));
      return 1;
    }
    if (fno.fattrib & AM_DIR) {
      sh_err(a, "Is a directory");
      return 1;
    }
    if (!t_container(path)) {
      sh_err(a, "cannot execute: not an Aurora app (see 'info')");
      return 1;
    }
  }
  {
    static FIL f;
    aos_header_t h;
    u32 n = 0;
    if (f_open(&f, path, FA_READ) == FR_OK) {
      n = info_read(&f, 0, &h, sizeof(h));
      f_close(&f);
    }
    if (n < sizeof(h) || !app_sane(&h, (u32)fno.fsize)) {
      sh_err(a, "cannot execute: the header points past the end of the file");
      return 1;
    }
  }
  if (!t_launch) {
    sh_err(a, "apps cannot be run from here");
    return 1;
  }

  t_out("Starting ", TC_DIM);
  t_line(t_base(path), TC_DIM);
  t_flush();
  t_unmount();
  t_launch(path + 3); /* the loader takes a path without the drive */
  /* Only back when the app did not start; the loader said why below. */
  t_need_sd(0);
  t_kb_redraw();
  sh_err(a, "the app did not start");
  return 1;
}

enum { U_CORE, U_AUDIO, U_TOUCH, U_GPU, U_WIFI, U_SD, U_POWER, U_COUNT };

static const char *const unit_name[U_COUNT] = {
    "core11.service", "audio.service", "touch.service", "gpu.service",
    "wifi.service",   "sdcard.mount",  "power.service"};
static const char *const unit_desc[U_COUNT] = {
    "ARM11 core",  "Sound (CSND)",   "Touchscreen",
    "PICA200 GPU", "Wi-Fi (AR6014)", "SD card",
    "Battery and clock (MCU)"};

/* 1 active, 0 inactive, -1 failed. `sub` is the state's detail in systemd's
 * words; `note` a few words about the unit. */
static int unit_state(int u, const char **sub, char *note) {
  int core = audio_alive();
  char *p = note;
  *p = 0;
  *sub = "dead";
  switch (u) {
    case U_CORE:
      if (!core) {
        t_cpy(p, "not answering");
        return -1;
      }
      *sub = "running";
      p = t_cpy(p, "core v");
      p = t_num(p, audio_version());
      if (audio_version() != AUDIO_CORE_VERSION) {
        p = t_cpy(p, ", this build has v");
        t_num(p, AUDIO_CORE_VERSION);
      }
      return 1;
    case U_AUDIO:
    case U_TOUCH:
      if (!core) {
        t_cpy(p, "the ARM11 core is down");
        return -1;
      }
      if (u == U_TOUCH) {
        *sub = "running";
        t_cpy(p, touch_cal_is_default() ? "default calibration"
                                        : "custom calibration");
        return 1;
      }
      if (audio_status() == AUDIO_ST_PARKED) {
        t_cpy(p, "parked");
        return 0;
      }
      *sub = "running";
      t_cpy(p, audio_status() == AUDIO_ST_PLAY ? "playing" : "idle");
      return 1;
    case U_GPU: {
      GpuShared g;
      if (!gpu_alive()) {
        t_cpy(p, "not answering");
        return -1;
      }
      gpu_get(&g);
      if (!g.ready) {
        t_cpy(p, "not set up");
        return 0;
      }
      *sub = "running";
      p = t_cpy(p, "ready, ");
      p = t_num(p, g.ops);
      t_cpy(p, " jobs done");
      return 1;
    }
    case U_WIFI: {
      static WifiShared w;
      if (!wifi_online()) {
        t_cpy(p, "not joined; ping joins the network saved in Settings");
        return 0;
      }
      wifi_get(&w);
      *sub = "running";
      p = t_cpy(p, "joined, address ");
      p = t_ip(p, w.ip);
      /* The SDIO clock is the 67 MHz bus clock over 4 x the divider. */
      if (w.bus_div && w.bus_div <= 0x20u) {
        u32 khz = 67028u / (w.bus_div * 4u);
        p = t_cpy(p, ", SDIO ");
        if (khz >= 1000u) {
          p = t_num(p, khz / 1000u);
          p = t_cpy(p, ".");
          p = t_num(p, khz % 1000u / 100u);
          p = t_cpy(p, " MHz");
        } else {
          p = t_num(p, khz);
          p = t_cpy(p, " kHz");
        }
        if (!w.bus_rx53)
          t_cpy(p, ", byte reads");
      }
      return 1;
    }
    case U_SD: {
      DWORD fre;
      FATFS *fs;
      if (!t_need_sd(0)) {
        t_cpy(p, "no card");
        return 0;
      }
      *sub = "mounted";
      if (f_getfree("0:", &fre, &fs) == FR_OK) {
        p = t_gb(p, (unsigned long long)fre * fs->csize);
        p = t_cpy(p, " of ");
        p = t_gb(p, (unsigned long long)(fs->n_fatent - 2u) * fs->csize);
        t_cpy(p, " GB free");
      }
      return 1;
    }
    default: {
      int pct = battery_percent();
      if (pct < 0) {
        t_cpy(p, "the MCU is not answering");
        return -1;
      }
      *sub = "running";
      p = t_cpy(p, "battery ");
      p = t_num(p, (u32)pct);
      p = t_cpy(p, "%");
      if (battery_charging() > 0)
        t_cpy(p, ", charging");
      return 1;
    }
  }
}

static const char *state_word(int st) {
  return st > 0 ? "active" : st < 0 ? "failed" : "inactive";
}

static int state_color(int st) {
  return st > 0 ? TC_EXEC : st < 0 ? TC_ERR : TC_DIM;
}

/* "audio" or "audio.service". */
static int unit_find(const char *s) {
  for (int u = 0; u < U_COUNT; u++) {
    const char *n = unit_name[u];
    int k = 0;
    while (s[k] && s[k] == n[k])
      k++;
    if (!s[k] && (!n[k] || n[k] == '.'))
      return u;
  }
  return -1;
}

/* An unknown unit as systemctl names it: ".service" added to a bare name. */
static void unit_missing(const char *s) {
  m_at = m_buf;
  m_add("Unit ");
  m_add(s);
  if (!strchr(s, '.'))
    m_add(".service");
  m_add(" could not be found.");
  m_end();
}

static void unit_status(int u) {
  static char note[64];
  const char *sub;
  int st = unit_state(u, &sub, note);
  t_outc('*', state_color(st));
  t_outc(' ', TC_TEXT);
  t_out(unit_name[u], TC_TEXT);
  t_out(" - ", TC_TEXT);
  t_line(unit_desc[u], TC_TEXT);
  t_out("   Loaded: ", TC_DIM);
  t_line("loaded (built in)", TC_TEXT);
  t_out("   Active: ", TC_DIM);
  t_out(state_word(st), state_color(st));
  t_out(" (", TC_TEXT);
  t_out(sub, TC_TEXT);
  t_line(")", TC_TEXT);
  t_out("   Status: ", TC_DIM);
  t_wrap(note, 11, TC_TEXT);
}

static void cmd_systemctl(int argc, char **argv) {
  static char note[64];
  const char *verb = argc > 1 ? argv[1] : "list-units";
  const char *sub;

  if (t_streq(verb, "list-units")) {
    t_line("UNIT            ACTIVE   DESCRIPTION", TC_DIM);
    for (int u = 0; u < U_COUNT; u++) {
      int st = unit_state(u, &sub, note);
      t_pad(unit_name[u], 16, TC_TEXT);
      t_pad(state_word(st), 9, state_color(st));
      t_wrap(note, 25, TC_TEXT);
    }
    t_outc('\n', TC_TEXT);
    t_u32(U_COUNT, TC_TEXT);
    t_line(" units listed.", TC_TEXT);
    return;
  }
  if (t_streq(verb, "poweroff") || t_streq(verb, "reboot")) {
    t_power(verb[0] == 'r');
    return;
  }
  if (t_streq(verb, "status")) {
    int failed = 0;
    for (int i = 2; i < argc; i++) {
      int u = unit_find(argv[i]);
      if (i > 2)
        t_outc('\n', TC_TEXT);
      if (u < 0)
        unit_missing(argv[i]);
      else
        unit_status(u);
    }
    if (argc > 2)
      return;
    for (int u = 0; u < U_COUNT; u++)
      if (unit_state(u, &sub, note) < 0)
        failed++;
    t_outc('*', failed ? TC_ERR : TC_EXEC);
    t_outc(' ', TC_TEXT);
    t_line(t_host, TC_TEXT);
    t_out("    State: ", TC_DIM);
    t_line(failed ? "degraded" : "running", failed ? TC_ERR : TC_EXEC);
    t_out("    Units: ", TC_DIM);
    t_u32(U_COUNT, TC_TEXT);
    t_out(" loaded, ", TC_TEXT);
    t_u32((u32)failed, TC_TEXT);
    t_line(" failed", TC_TEXT);
    t_out("    Model: ", TC_DIM);
    t_line(aurora_is_new3ds() ? "New 3DS" : "Old 3DS", TC_TEXT);
    t_out("   System: ", TC_DIM);
    t_out("AuroraOS ", TC_TEXT);
    t_line(AURORA_VERSION, TC_TEXT);
    return;
  }
  if (t_streq(verb, "is-active")) {
    for (int i = 2; i < argc; i++) {
      int u = unit_find(argv[i]);
      if (u < 0)
        t_line("unknown", TC_DIM);
      else {
        int st = unit_state(u, &sub, note);
        t_line(state_word(st), state_color(st));
      }
    }
    return;
  }
  if (t_streq(verb, "start") || t_streq(verb, "stop") ||
      t_streq(verb, "restart") || t_streq(verb, "enable") ||
      t_streq(verb, "disable")) {
    if (argc < 3) {
      t_line("Too few arguments.", TC_ERR);
      return;
    }
    for (int i = 2; i < argc; i++) {
      int u = unit_find(argv[i]);
      if (u == U_AUDIO && t_streq(verb, "stop")) {
        audio_voices_stop();
        audio_stop();
        continue;
      }
      m_at = m_buf;
      m_add("Failed to ");
      m_add(verb);
      m_add(" ");
      m_add(u < 0 ? argv[i] : unit_name[u]);
      if (u < 0 && !strchr(argv[i], '.'))
        m_add(".service");
      m_add(u < 0 ? ": Unit not found." : ": Operation not supported.");
      m_end();
    }
    return;
  }
  m_start("systemctl");
  m_add("Unknown command verb ");
  m_q(verb);
  m_add(".");
  m_end();
}

/* ping: echo requests over the network the core keeps joined (WiFi9.c). */
static char net_ssid[33];
static u32 net_shown;

static void net_tick(u32 ms) {
  static char line[64];
  if (ms / 1000u == net_shown)
    return;
  net_shown = ms / 1000u;
  char *p = t_cpy(line, "Joining ");
  p = t_cpy(p, net_ssid[0] ? net_ssid : "the saved network");
  p = t_cpy(p, "... ");
  p = t_num(p, net_shown);
  t_cpy(p, " s");
  t_busy(line);
}

/* The network the core keeps up, joined first if it is not. 0, after saying
 * why as `cmd`, when there is none. */
static int net_join(const char *cmd) {
  static WifiShared w;
  static char why[96], line[96];
  WifiNetResult r;
  if (wifi_online())
    return 1;
  /* The core may still hold a join the OS forgot, after an app. */
  if (wifi_net(WIFI_NETOP_STATUS, 0, 0, 0, 1000u, &r, 0) == WIFI_NETS_OK)
    return 1;
  net_ssid[0] = 0;
  net_shown = 0xFFFFFFFFu;
  t_unmount(); /* staging the firmware mounts the card */
  int ok = wifi_net_join(net_ssid, sizeof(net_ssid), why, sizeof(why),
                         net_tick);
  t_need_sd(0);
  if (!ok) {
    t_err(cmd, 0, 0, why);
    return 0;
  }
  wifi_get(&w);
  char *q = t_cpy(line, "Joined ");
  q = t_cpy(q, net_ssid);
  q = t_cpy(q, ", address ");
  t_ip(q, w.ip);
  t_line(line, TC_DIM);
  return 1;
}

static int t_parse_ip(const char *s, u32 *ip) {
  u32 v = 0;
  for (int i = 0; i < 4; i++) {
    u32 x = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
      x = x * 10u + (u32)(*s++ - '0');
      if (++digits > 3)
        return 0;
    }
    if (!digits || x > 255u || (i < 3 && *s++ != '.'))
      return 0;
    v = (v << 8) | x;
  }
  if (*s)
    return 0;
  *ip = v;
  return 1;
}

static int t_parse_u32(const char *s, u32 *v) {
  u32 x = 0;
  if (!*s)
    return 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || x > 100000u)
      return 0;
    x = x * 10u + (u32)(*s - '0');
  }
  *v = x;
  return 1;
}

/* Milliseconds as ping prints a round trip: 0.123, 1.23, 12.3 or 123. */
static char *t_ms(char *p, u32 us) {
  u32 ms = us / 1000u, f = us % 1000u;
  p = t_num(p, ms);
  if (us >= 100000u)
    return p;
  *p++ = '.';
  if (us >= 10000u) {
    *p++ = (char)('0' + f / 100u);
  } else if (us >= 1000u) {
    p = t_two(p, f / 10u);
  } else {
    *p++ = (char)('0' + f / 100u);
    p = t_two(p, f % 100u);
  }
  *p = 0;
  return p;
}

/* Three decimals, for the summary. */
static char *t_ms3(char *p, u32 us) {
  p = t_num(p, us / 1000u);
  *p++ = '.';
  *p++ = (char)('0' + us % 1000u / 100u);
  return t_two(p, us % 100u);
}

static u32 isqrt64(unsigned long long v) {
  unsigned long long r = 0, bit = 1ull << 62;
  while (bit > v)
    bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return (u32)r;
}

static const char *icmp_text(u32 icmp) {
  u32 type = icmp >> 8, code = icmp & 0xFFu;
  if (type == 11u)
    return "Time to live exceeded";
  switch (code) {
    case 0: return "Destination Net Unreachable";
    case 1: return "Destination Host Unreachable";
    case 2: return "Destination Protocol Unreachable";
    case 3: return "Destination Port Unreachable";
    case 4: return "Frag needed";
    case 13: return "Packet filtered";
    default: return "Destination Unreachable";
  }
}

static void cmd_ping(int argc, char **argv) {
  static WifiShared w;
  static char line[128];
  WifiNetResult r;
  const char *host = 0;
  u32 count = 0, wait_s = 2, ip = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if ((t_streq(a, "-c") || t_streq(a, "-W")) && i + 1 < argc) {
      u32 v;
      if (!t_parse_u32(argv[++i], &v) || !v) {
        t_err("ping", "invalid argument:", argv[i], 0);
        return;
      }
      if (a[1] == 'c')
        count = v;
      else
        wait_s = v > 10u ? 10u : v;
    } else if (a[0] == '-') {
      t_err("ping", "invalid option", a, 0);
      t_line("usage: ping [-c COUNT] [-W SECONDS] HOST", TC_TEXT);
      return;
    } else if (host) {
      t_err("ping", 0, 0, "usage error: one destination only");
      return;
    } else {
      host = a;
    }
  }
  if (!host) {
    t_err("ping", 0, 0, "usage error: Destination address required");
    return;
  }
  if (!net_join("ping"))
    return;

  if (!t_parse_ip(host, &ip)) {
    u32 st = wifi_net(WIFI_NETOP_DNS, 0, 0, host, 2000u, &r, 0);
    if (st == WIFI_NETS_NOLINK && net_join("ping"))
      st = wifi_net(WIFI_NETOP_DNS, 0, 0, host, 2000u, &r, 0);
    if (st != WIFI_NETS_OK) {
      t_err("ping", host, 0,
            st == WIFI_NETS_NONAME || st == WIFI_NETS_BADNAME
                ? "Name or service not known"
            : st == WIFI_NETS_DNSFAIL ? "No address associated with hostname"
                                      : "Temporary failure in name resolution");
      if (st == WIFI_NETS_NOLINK)
        t_line("the Wi-Fi link dropped", TC_DIM);
      return;
    }
    ip = r.ip;
  }

  char *q = t_cpy(line, "PING ");
  q = t_cpy(q, host);
  q = t_cpy(q, " (");
  q = t_ip(q, ip);
  t_cpy(q, ") 56(84) bytes of data.");
  t_line(line, TC_TEXT);
  t_flush();

  u32 sent = 0, got = 0, errors = 0, tmin = 0xFFFFFFFFu, tmax = 0;
  unsigned long long tsum = 0, tsum2 = 0;
  u32 start = timer_ticks(), elapsed = 0;
  int stop = 0;
  for (u32 seq = 1; !stop; seq++) {
    u32 t0 = timer_ticks();
    u32 st = wifi_net(WIFI_NETOP_PING, ip, seq, 0, wait_s * 1000u, &r, 0);
    sent++;
    elapsed = timer_us_since(start) / 1000u;
    if (st == WIFI_NETS_OK) {
      got++;
      tsum += r.rtt_us;
      tsum2 += (unsigned long long)r.rtt_us * r.rtt_us;
      if (r.rtt_us < tmin)
        tmin = r.rtt_us;
      if (r.rtt_us > tmax)
        tmax = r.rtt_us;
      q = t_num(line, r.bytes);
      q = t_cpy(q, " bytes from ");
      q = t_ip(q, r.from);
      q = t_cpy(q, ": icmp_seq=");
      q = t_num(q, seq);
      q = t_cpy(q, " ttl=");
      q = t_num(q, r.ttl);
      q = t_cpy(q, " time=");
      q = t_ms(q, r.rtt_us);
      t_cpy(q, " ms");
      t_line(line, TC_TEXT);
    } else if (st == WIFI_NETS_ICMPERR || st == WIFI_NETS_NOHOST) {
      errors++;
      if (st == WIFI_NETS_NOHOST)
        wifi_get(&w);
      q = t_cpy(line, "From ");
      q = t_ip(q, st == WIFI_NETS_NOHOST ? w.ip : r.from);
      q = t_cpy(q, " icmp_seq=");
      q = t_num(q, seq);
      q = t_cpy(q, " ");
      t_cpy(q, st == WIFI_NETS_NOHOST ? "Destination Host Unreachable"
                                      : icmp_text(r.icmp));
      t_line(line, TC_TEXT);
    } else if (st == WIFI_NETS_TIMEOUT) {
      q = t_cpy(line, "no answer yet for icmp_seq=");
      t_num(q, seq);
      t_line(line, TC_DIM);
    } else if (st == WIFI_NETS_NOSEND) {
      t_err("ping", 0, 0, "sendmsg: No buffer space available");
    } else {
      t_err("ping", 0, 0,
            st == WIFI_NETS_NOLINK ? "the Wi-Fi link dropped"
                                   : "the Wi-Fi chip is busy");
      break;
    }
    t_flush();
    if (count && seq >= count)
      break;
    /* One a second, as ping sends them; SELECT stops, like Ctrl+C. */
    while (!stop && timer_us_since(t0) < 1000000u) {
      stop = !t_busy(0);
      ui_idle();
    }
  }
  if (stop)
    t_line("^C", TC_TEXT);

  q = t_cpy(line, "--- ");
  q = t_cpy(q, host);
  t_cpy(q, " ping statistics ---");
  t_line(line, TC_TEXT);
  q = t_num(line, sent);
  q = t_cpy(q, " packets transmitted, ");
  q = t_num(q, got);
  q = t_cpy(q, " received, ");
  if (errors) {
    q = t_cpy(q, "+");
    q = t_num(q, errors);
    q = t_cpy(q, " errors, ");
  }
  q = t_num(q, sent ? (sent - got) * 100u / sent : 0u);
  q = t_cpy(q, "% packet loss, time ");
  q = t_num(q, elapsed);
  t_cpy(q, "ms");
  t_line(line, TC_TEXT);
  if (got) {
    u32 avg = (u32)(tsum / got);
    unsigned long long mean2 = tsum2 / got, sq = (unsigned long long)avg * avg;
    q = t_cpy(line, "rtt min/avg/max/mdev = ");
    q = t_ms3(q, tmin);
    *q++ = '/';
    q = t_ms3(q, avg);
    *q++ = '/';
    q = t_ms3(q, tmax);
    *q++ = '/';
    q = t_ms3(q, mean2 > sq ? isqrt64(mean2 - sq) : 0u);
    t_cpy(q, " ms");
    t_line(line, TC_TEXT);
  }
}

static void cmd_shutdown(int argc, char **argv) {
  static const char fl[] = "hPrc";
  u32 on;
  int i = t_flags("shutdown", argc, argv, fl, &on);
  if (i < 0)
    return;
  if (has(on, fl, 'c')) {
    t_line("No shutdown is scheduled.", TC_TEXT);
    return;
  }
  if (i < argc && !t_streq(argv[i], "now") && !t_streq(argv[i], "+0")) {
    t_err("shutdown", 0, 0, "only 'now' is supported; nothing was scheduled");
    return;
  }
  t_power(has(on, fl, 'r'));
}

static void cmd_reboot(int argc, char **argv) {
  (void)argc;
  (void)argv;
  t_power(1);
}

static void cmd_poweroff(int argc, char **argv) {
  (void)argc;
  (void)argv;
  t_power(0);
}

static void cmd_whoami(int argc, char **argv) {
  if (argc > 1) {
    t_err("whoami", "extra operand", argv[1], 0);
    return;
  }
  t_line(t_user, TC_TEXT);
}

static void cmd_uname(int argc, char **argv) {
  static const char fl[] = "asnrmo";
  const char *part[5] = {"AuroraOS", t_host, AURORA_VERSION, "armv5tel",
                         "AuroraOS"};
  u32 on;
  int i = t_flags("uname", argc, argv, fl, &on), first = 1;
  if (i < 0)
    return;
  if (i < argc) {
    t_err("uname", "extra operand", argv[i], 0);
    return;
  }
  if (!on)
    on = 1u << 1;
  for (int k = 0; k < 5; k++) {
    if (!has(on, fl, 'a') && !has(on, fl, "snrmo"[k]))
      continue;
    if (!first)
      t_outc(' ', TC_TEXT);
    t_out(part[k], TC_TEXT);
    first = 0;
  }
  t_outc('\n', TC_TEXT);
}

static void cmd_date(int argc, char **argv) {
  static const char *const wday[7] = {"Sun", "Mon", "Tue", "Wed",
                                      "Thu", "Fri", "Sat"};
  static const char *const month[12] = {"Jan", "Feb", "Mar", "Apr",
                                        "May", "Jun", "Jul", "Aug",
                                        "Sep", "Oct", "Nov", "Dec"};
  RtcTime t;
  char buf[32], *p;
  (void)argc;
  (void)argv;
  if (!rtc_read(&t)) {
    t_err("date", 0, 0, "cannot read the clock");
    return;
  }
  p = t_cpy(buf, wday[t.wday % 7]);
  *p++ = ' ';
  p = t_cpy(p, month[t.month - 1]);
  *p++ = ' ';
  *p++ = t.day < 10 ? ' ' : (char)('0' + t.day / 10);
  *p++ = (char)('0' + t.day % 10);
  *p++ = ' ';
  p = t_two(p, (u32)t.hour);
  *p++ = ':';
  p = t_two(p, (u32)t.min);
  *p++ = ':';
  p = t_two(p, (u32)t.sec);
  *p++ = ' ';
  t_num(p, (u32)t.year);
  t_line(buf, TC_TEXT);
}

static void cmd_echo(int argc, char **argv) {
  int i = 1, nl = 1;
  if (argc > 1 && t_streq(argv[1], "-n")) {
    nl = 0;
    i = 2;
  }
  for (; i < argc; i++) {
    t_out(argv[i], TC_TEXT);
    if (i < argc - 1)
      t_outc(' ', TC_TEXT);
  }
  if (nl)
    t_outc('\n', TC_TEXT);
}

static void cmd_clear(int argc, char **argv) {
  (void)argc;
  (void)argv;
  t_clear();
}

static void cmd_exit(int argc, char **argv) {
  (void)argc;
  (void)argv;
  t_quit = 1;
}

void t_help(const TermCmd *c) {
  t_out("usage: ", TC_TEXT);
  t_line(c->usage, TC_TEXT);
  t_wrap(c->about, 0, TC_TEXT);
  if (c->more)
    t_wrap(c->more, 0, TC_DIM);
}

static void cmd_help(int argc, char **argv) {
  if (argc > 1) {
    for (int i = 0; i < t_ncmds; i++)
      if (t_streq(argv[1], t_cmds[i].name)) {
        t_help(&t_cmds[i]);
        return;
      }
    t_err("help", "no command called", argv[1], 0);
    return;
  }
  t_line("Commands (help NAME says more):", TC_TEXT);
  for (int i = 0; i < t_ncmds; i++) {
    t_out("  ", TC_TEXT);
    t_pad(t_cmds[i].name, 10, TC_EXEC);
    t_wrap(t_cmds[i].about, 12, TC_TEXT);
  }
  t_wrap("Run an app by its path (./Tetris.bin) or by its name in "
         "/Aurora/Apps or /Aurora/Apps/C (Tetris). Tab completes names; * "
         "and ? match them.",
         0, TC_DIM);
  t_wrap("Keys: A Enter, B Del, Y Tab, L Shift, R symbols, Up and Down for "
         "history, Left and Right to move, SELECT cancels, START closes.",
         0, TC_DIM);
}

const TermCmd t_cmds[] = {
    {"cat", cmd_cat, "cat FILE...", "print files",
     "SELECT stops a long file."},
    {"cd", cmd_cd, "cd [DIR]", "change the working folder",
     "With no folder, or ~, goes to the root of the card; cd - goes back to "
     "the last one."},
    {"clear", cmd_clear, "clear", "clear the screen", 0},
    {"cp", cmd_cp, "cp [-rn] SRC... DEST", "copy files and folders",
     "-r copies folders, -n keeps files already there. Otherwise a file in "
     "the way is replaced. SELECT stops a long copy."},
    {"date", cmd_date, "date", "print the date and time", 0},
    {"echo", cmd_echo, "echo [-n] [TEXT]...", "print text",
     "-n leaves out the newline."},
    {"exit", cmd_exit, "exit", "close the terminal", 0},
    {"help", cmd_help, "help [COMMAND]", "list the commands, or explain one",
     0},
    {"info", cmd_info, "info FILE...", "what Aurora can do with a file",
     "Whether Aurora can execute it (an AOS1, AUR1 or AURC app) and what "
     "opens it, with picture sizes, sound formats and app headers; for a C "
     "app, its data folder."},
    {"ls", cmd_ls, "ls [-la1] [PATH]...", "list a folder",
     "-l long listing, -a hidden files too, -1 one per line. Folders are "
     "blue, apps green, pictures magenta and sound cyan."},
    {"mkdir", cmd_mkdir, "mkdir [-p] DIR...", "make folders",
     "-p makes the parents too, and is fine with folders already there."},
    {"mv", cmd_mv, "mv [-n] SRC... DEST", "move or rename",
     "-n keeps files already there. Otherwise a file in the way is "
     "replaced."},
    {"ping", cmd_ping, "ping [-c COUNT] [-W SECONDS] HOST",
     "send echo requests to a host over Wi-Fi",
     "HOST is a name or an address. If the console is not on the network, "
     "ping first joins the one saved in Settings > Wi-Fi (open or WPA2), "
     "which takes about half a minute. One echo a second "
     "until SELECT, or COUNT of them; -W is how long to wait for each "
     "reply (2 seconds). Times include the console's polling, so they read "
     "a little high."},
    {"poweroff", cmd_poweroff, "poweroff", "turn the console off", 0},
    {"pwd", cmd_pwd, "pwd", "print the working folder", 0},
    {"reboot", cmd_reboot, "reboot", "restart the console", 0},
    {"rm", cmd_rm, "rm [-rf] PATH...", "remove files and folders",
     "-r removes folders and all they hold, -f ignores missing files. "
     "SELECT stops a long removal."},
    {"shutdown", cmd_shutdown, "shutdown [-h|-P|-r] [now]",
     "turn off, or restart with -r",
     "-h and -P turn the console off, -r restarts it. Only 'now' is "
     "supported as a time."},
    {"systemctl", cmd_systemctl, "systemctl [VERB] [UNIT]...",
     "look at Aurora's services",
     "Verbs: list-units, status, is-active, stop, poweroff, reboot. Units: "
     "core11, audio, touch, gpu, wifi, sdcard, power. 'stop audio' silences "
     "anything playing."},
    {"touch", cmd_touch, "touch FILE...", "make empty files", 0},
    {"uname", cmd_uname, "uname [-asnrmo]", "print system information",
     "-s system, -n host, -r release, -m machine, -o OS, -a all of them."},
    {"whoami", cmd_whoami, "whoami", "print the user name", 0},
};

const int t_ncmds = (int)(sizeof(t_cmds) / sizeof(t_cmds[0]));
