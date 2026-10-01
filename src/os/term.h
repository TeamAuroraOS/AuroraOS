#ifndef AURORA_TERM_H
#define AURORA_TERM_H

/* Shared by the terminal's screen and shell (Terminal.c) and its commands
 * (TermCmds.c). Paths are FatFs paths, "0:/Aurora/Apps"; the shell shows them
 * without the "0:". */

#include "aurora.h"
#include "ff.h"
#include "fileops.h"

#define T_PATH  FO_PATH
#define T_INPUT 256 /* one command line, including the NUL */
#define T_COLS  49

/* Output colours; Terminal.c maps them to RGB. */
enum {
  TC_TEXT = 0,
  TC_DIM,
  TC_USER, /* user@host in the prompt: the accent colour */
  TC_DIR,
  TC_EXEC, /* Aurora apps, and "active" */
  TC_IMAGE,
  TC_AUDIO,
  TC_ERR,
  TC_WARN,
  TC_COUNT
};

/* Text goes to the scrollback; '\n' ends a line and long lines wrap. */
void t_out(const char *s, int color);
void t_outc(char c, int color);
void t_line(const char *s, int color);
void t_u32(u32 v, int color);
/* `s` and then spaces, up to `width` columns. */
void t_pad(const char *s, int width, int color);
/* `s` wrapped at spaces, its later lines starting at column `indent`, and a
 * newline. */
void t_wrap(const char *s, int indent, int color);
/* Column-major, as ls lays out names. */
void t_columns(const char *const *names, const u8 *colors, int n);
/* "cmd: pre 'what': msg"; any of the last three may be 0. */
void t_err(const char *cmd, const char *pre, const char *what,
           const char *msg);
void t_clear(void);
/* Columns a string takes: one per character, not per UTF-8 byte. */
int t_width(const char *s);

/* For a long job: shows `s` (0 for none) below the output, every few frames.
 * Returns 0 while SELECT is held, to stop the job. */
int t_busy(const char *s);

void t_flush(void);
/* After something else drew on the bottom screen. */
void t_kb_redraw(void);

extern char t_cwd[T_PATH];
extern char t_oldcwd[T_PATH];
extern const char *t_user;
extern const char *t_host;
extern void (*t_launch)(const char *path);
extern int t_quit;

/* `arg` against the working folder, with "." and ".." applied, into `out`.
 * "~" is the card's root. Returns 0 when the result would not fit. */
int t_resolve(const char *arg, char *out);
/* `dir` and `name` with one slash between; 0 when that would not fit. */
int t_join(char *out, const char *dir, const char *name);
/* The shell's view of a FatFs path: "0:/a" is "/a". */
const char *t_show(const char *path);
int t_is_root(const char *path);
/* Mounts the card if it is not. 0, after saying so as `cmd`, without one. */
int t_need_sd(const char *cmd);
void t_unmount(void);

/* f_stat that also answers for the root, which FatFs refuses. */
FRESULT t_stat(const char *path, FILINFO *fno);
const char *t_frerr(FRESULT r);
const char *t_foerr(FoResult r);

/* A folder's entries, sorted by name. */
typedef struct {
  const char *name;
  u32 size;
  u16 date, time;
  u8 attr;
  u8 kind; /* FileKind; FKIND_AURORA only when read with `kinds` */
} TEnt;

#define T_ENTS 1024
extern TEnt t_ents[T_ENTS];
extern int t_ents_cut; /* the folder held more than fits */

/* Returns the count, or -1 with *fr set when the folder cannot be read.
 * Hidden and system entries, and names starting with a dot, are left out
 * unless `all`. With `kinds`, each .bin is opened to find the apps. */
int t_readdir(const char *dir, int all, int kinds, FRESULT *fr);

/* 'A' for an AOS1 container, 'U' for AUR1, 0 for anything else. */
int t_container(const char *path);

typedef struct {
  const char *name;
  void (*run)(int argc, char **argv);
  const char *usage;
  const char *about; /* one line, for help's list */
  const char *more;  /* for help NAME and --help; may be 0 */
} TermCmd;

extern const TermCmd t_cmds[];
extern const int t_ncmds;

void t_help(const TermCmd *c);

/* Runs argv[0] as an app when it names one. 0 when it does not, so the shell
 * can report the command as not found. */
int t_exec(int argc, char **argv);

int t_streq(const char *a, const char *b);
int t_icmp(const char *a, const char *b); /* ignoring ASCII case */
char t_fold(char c);
char *t_cpy(char *dst, const char *src);

#endif
