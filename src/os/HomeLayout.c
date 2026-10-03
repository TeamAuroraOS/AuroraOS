#include "homelayout.h"
#include "ff.h"

typedef struct {
  int n;
  s16 e[HOME_ITEMS];
} List;

typedef struct {
  int used;
  int parent; /* a container, or -2 while the folder is held nowhere */
  char name[HOME_NAME];
  List l;
} Folder;

static List root;
static Folder fold[HOME_FOLDERS];

#define NOWHERE (-2)

static List *list_of(int c) {
  if (c == HOME_ROOT)
    return &root;
  if (c >= 0 && c < HOME_FOLDERS && fold[c].used)
    return &fold[c].l;
  return 0;
}

static void set_name(char *dst, const char *src) {
  int n = 0;
  while (src[n] && n < HOME_NAME - 1)
    n++;
  /* back off to a whole UTF-8 character */
  while (n > 0 && src[n] && ((u8)src[n] & 0xC0u) == 0x80u)
    n--;
  for (int i = 0; i < n; i++)
    dst[i] = src[i];
  dst[n] = 0;
}

void hl_reset(int napps) {
  if (napps > HOME_ITEMS)
    napps = HOME_ITEMS;
  for (int f = 0; f < HOME_FOLDERS; f++)
    fold[f].used = 0;
  root.n = napps;
  for (int i = 0; i < napps; i++)
    root.e[i] = (s16)i;
}

int hl_count(int c) {
  const List *l = list_of(c);
  return l ? l->n : 0;
}

int hl_at(int c, int i) {
  const List *l = list_of(c);
  return (l && i >= 0 && i < l->n) ? l->e[i] : HL_NONE;
}

int hl_index(int c, int e) {
  const List *l = list_of(c);
  if (l)
    for (int i = 0; i < l->n; i++)
      if (l->e[i] == e)
        return i;
  return -1;
}

int hl_depth(int c) {
  int d = 0;
  while (c != HOME_ROOT && c >= 0 && d <= HOME_DEPTH) {
    d++;
    c = fold[c].parent;
  }
  return d;
}

int hl_height(int e) {
  int f, h = 0;
  if (!HL_IS_FOLDER(e))
    return 0;
  f = HL_FOLDER_ID(e);
  for (int i = 0; i < fold[f].l.n; i++) {
    int k = hl_height(fold[f].l.e[i]);
    if (k > h)
      h = k;
  }
  return h + 1;
}

int hl_parent(int f) { return fold[f].parent; }

const char *hl_name(int f) {
  return (f >= 0 && f < HOME_FOLDERS && fold[f].used) ? fold[f].name : "";
}

int hl_folders_free(void) {
  int n = 0;
  for (int f = 0; f < HOME_FOLDERS; f++)
    n += !fold[f].used;
  return n;
}

int hl_fits(int c, int e) {
  const List *l = list_of(c);
  if (!l || l->n >= HOME_ITEMS)
    return HL_FULL;
  if (HL_IS_FOLDER(e)) {
    for (int k = c; k != HOME_ROOT && k >= 0; k = fold[k].parent)
      if (k == HL_FOLDER_ID(e))
        return HL_INSIDE;
  }
  if (hl_depth(c) + hl_height(e) > HOME_DEPTH)
    return HL_TOO_DEEP;
  return HL_OK;
}

int hl_take(int c, int i) {
  List *l = list_of(c);
  int e;
  if (!l || i < 0 || i >= l->n)
    return HL_NONE;
  e = l->e[i];
  for (int k = i; k + 1 < l->n; k++)
    l->e[k] = l->e[k + 1];
  l->n--;
  if (HL_IS_FOLDER(e))
    fold[HL_FOLDER_ID(e)].parent = NOWHERE;
  return e;
}

/* No checks: the callers have made them. */
static void insert(int c, int i, int e) {
  List *l = list_of(c);
  if (i < 0)
    i = 0;
  if (i > l->n)
    i = l->n;
  for (int k = l->n; k > i; k--)
    l->e[k] = l->e[k - 1];
  l->e[i] = (s16)e;
  l->n++;
  if (HL_IS_FOLDER(e))
    fold[HL_FOLDER_ID(e)].parent = c;
}

int hl_put(int c, int i, int e) {
  int r = hl_fits(c, e);
  if (r == HL_OK)
    insert(c, i, e);
  return r;
}

static int alloc_folder(const char *name) {
  for (int f = 0; f < HOME_FOLDERS; f++)
    if (!fold[f].used) {
      fold[f].used = 1;
      fold[f].parent = NOWHERE;
      fold[f].l.n = 0;
      set_name(fold[f].name, name);
      return f;
    }
  return HL_NO_FOLDERS;
}

int hl_new_folder(int c, int i, const char *name) {
  const List *l = list_of(c);
  int f;
  if (!l || l->n >= HOME_ITEMS)
    return HL_FULL;
  if (hl_depth(c) + 1 > HOME_DEPTH)
    return HL_TOO_DEEP;
  f = alloc_folder(name);
  if (f >= 0)
    insert(c, i, HL_FOLDER(f));
  return f;
}

int hl_wrap(int c, int i, const char *name) {
  int e = hl_at(c, i), f;
  if (e == HL_NONE)
    return HL_FULL;
  if (hl_depth(c) + 1 + hl_height(e) > HOME_DEPTH)
    return HL_TOO_DEEP;
  f = alloc_folder(name);
  if (f < 0)
    return f;
  hl_take(c, i);
  insert(c, i, HL_FOLDER(f));
  insert(f, 0, e);
  return f;
}

int hl_merge(int c, int i, int e, const char *name) {
  int t = hl_at(c, i), f;
  if (t == HL_NONE || HL_IS_FOLDER(t))
    return HL_FULL;
  if (hl_depth(c) + 1 + hl_height(e) > HOME_DEPTH)
    return HL_TOO_DEEP;
  f = hl_wrap(c, i, name);
  if (f >= 0)
    insert(f, 1, e);
  return f;
}

int hl_unfold(int f) {
  int p = fold[f].parent, at = hl_index(p, HL_FOLDER(f)), n = fold[f].l.n;
  if (at < 0)
    return HL_FULL;
  if (hl_count(p) - 1 + n > HOME_ITEMS)
    return HL_FULL;
  hl_take(p, at);
  for (int k = 0; k < n; k++)
    insert(p, at + k, fold[f].l.e[k]);
  fold[f].used = 0;
  return HL_OK;
}

void hl_rename(int f, const char *name) {
  if (f >= 0 && f < HOME_FOLDERS && fold[f].used)
    set_name(fold[f].name, name);
}

/* The file: one line per entry, in order. "A key" is an app, "F name" opens a
 * folder and "E" closes it. Lines starting with '#' are notes. */

#define HL_FILE_MAX (64u * 1024u)
static char buf[HL_FILE_MAX];
static u8 placed[HOME_ITEMS + 8];

static int same(const char *a, int n, const char *b) {
  for (int i = 0; i < n; i++)
    if (a[i] != b[i] || !b[i])
      return 0;
  return b[n] == 0;
}

static void parse(char *s, u32 len, const char *const *keys, int napps) {
  /* The containers opened by "F" lines; -3 marks one refused (too deep, no
   * room or no folders left), whose entries stay in the container around it. */
  int stack[HOME_DEPTH + 8], sp = 0, c = HOME_ROOT;
  char *end = s + len;

  while (s < end) {
    char *line = s, *eol = s;
    int n;
    while (eol < end && *eol != '\n')
      eol++;
    s = eol + 1;
    n = (int)(eol - line);
    while (n > 0 && line[n - 1] == '\r')
      n--;
    if (n < 1 || line[0] == '#')
      continue;

    if (line[0] == 'E' && n == 1) {
      if (sp > 0) {
        sp--;
        if (stack[sp] != -3)
          c = stack[sp];
      }
    } else if (line[0] == 'F' && (n == 1 || line[1] == ' ')) {
      char name[HOME_NAME * 2]; /* set_name() cuts it at a whole character */
      int f, k = n - 2 < HOME_NAME * 2 - 1 ? n - 2 : HOME_NAME * 2 - 1;
      if (k < 0)
        k = 0;
      if (sp >= (int)(sizeof(stack) / sizeof(stack[0])))
        continue;
      for (int i = 0; i < k; i++)
        name[i] = line[2 + i];
      name[k] = 0;
      f = hl_new_folder(c, hl_count(c), name);
      if (f >= 0) {
        stack[sp++] = c;
        c = f;
      } else {
        stack[sp++] = -3;
      }
    } else if (line[0] == 'A' && n >= 2 && line[1] == ' ') {
      for (int a = 0; a < napps; a++)
        if (!placed[a] && same(line + 2, n - 2, keys[a])) {
          if (hl_put(c, hl_count(c), a) == HL_OK)
            placed[a] = 1;
          break;
        }
    }
  }
}

int hl_load(const char *const *keys, int napps) {
  static FATFS fs;
  static FIL f;
  UINT br = 0;
  int ok = 0;

  if (napps > HOME_ITEMS)
    napps = HOME_ITEMS;
  hl_reset(0);
  for (int a = 0; a < napps; a++)
    placed[a] = 0;
  if (f_mount(&fs, "", 1) == FR_OK) {
    if (f_open(&f, HOME_FILE, FA_READ) == FR_OK) {
      ok = f_read(&f, buf, HL_FILE_MAX, &br) == FR_OK;
      f_close(&f);
    }
    f_mount(NULL, "", 0);
  }
  if (ok)
    parse(buf, br, keys, napps);
  for (int a = 0; a < napps; a++)
    if (!placed[a] && root.n < HOME_ITEMS)
      root.e[root.n++] = (s16)a;
  return ok;
}

static char *put(char *p, const char *s) {
  while (*s && p < buf + HL_FILE_MAX - 1)
    *p++ = *s++;
  return p;
}

static char *write_list(char *p, int c, const char *const *keys) {
  for (int i = 0; i < hl_count(c); i++) {
    int e = hl_at(c, i);
    if (HL_IS_FOLDER(e)) {
      p = put(p, "F ");
      p = put(p, fold[HL_FOLDER_ID(e)].name);
      p = put(p, "\n");
      p = write_list(p, HL_FOLDER_ID(e), keys);
      p = put(p, "E\n");
    } else {
      p = put(p, "A ");
      p = put(p, keys[e]);
      p = put(p, "\n");
    }
  }
  return p;
}

int hl_save(const char *const *keys, int napps) {
  static FATFS fs;
  static FIL f;
  char *p = put(buf, "# Aurora Home Menu: A app, F folder, E end of folder\n");
  UINT bw = 0;
  int ok = 0;

  (void)napps;
  p = write_list(p, HOME_ROOT, keys);
  if (f_mount(&fs, "", 1) != FR_OK)
    return 0;
  if (f_open(&f, HOME_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    ok = f_write(&f, buf, (UINT)(p - buf), &bw) == FR_OK &&
         bw == (UINT)(p - buf);
    f_close(&f);
  }
  f_mount(NULL, "", 0);
  return ok;
}
