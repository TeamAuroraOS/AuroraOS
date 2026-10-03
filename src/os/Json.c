/* A small JSON reader for glTF: tokens are laid out in document order, each
 * knowing where its subtree ends, so lookups skip whole subtrees without
 * recursion. */
#include "json.h"

typedef struct {
  Json *j;
  u32 pos, len;
  int depth;
} Parser;

#define MAX_DEPTH 64

static void skip_ws(Parser *p) {
  while (p->pos < p->len) {
    char c = p->j->src[p->pos];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;
    p->pos++;
  }
}

static int add(Parser *p, u8 type, u32 start) {
  Json *j = p->j;
  if (j->n >= j->cap)
    return -1;
  j->tok[j->n].type = type;
  j->tok[j->n].start = start;
  j->tok[j->n].end = start;
  j->tok[j->n].count = 0;
  j->tok[j->n].next = 0;
  return (int)j->n++;
}

static int value(Parser *p);

static int string(Parser *p) {
  const char *s = p->j->src;
  int t;
  p->pos++; /* the opening quote */
  t = add(p, JSON_STR, p->pos);
  if (t < 0)
    return 0;
  while (p->pos < p->len && s[p->pos] != '"') {
    if (s[p->pos] == '\\')
      p->pos++;
    p->pos++;
  }
  if (p->pos >= p->len)
    return 0;
  p->j->tok[t].end = p->pos++;
  p->j->tok[t].next = p->j->n;
  return 1;
}

static int container(Parser *p, int obj) {
  int t = add(p, obj ? JSON_OBJ : JSON_ARR, p->pos);
  char close = obj ? '}' : ']';
  if (t < 0 || ++p->depth > MAX_DEPTH)
    return 0;
  p->pos++;
  skip_ws(p);
  if (p->pos < p->len && p->j->src[p->pos] == close) {
    p->pos++;
  } else {
    for (;;) {
      skip_ws(p);
      if (obj) {
        if (p->pos >= p->len || p->j->src[p->pos] != '"' || !string(p))
          return 0;
        skip_ws(p);
        if (p->pos >= p->len || p->j->src[p->pos] != ':')
          return 0;
        p->pos++;
      }
      if (!value(p))
        return 0;
      p->j->tok[t].count++;
      skip_ws(p);
      if (p->pos >= p->len)
        return 0;
      if (p->j->src[p->pos] == ',') {
        p->pos++;
        continue;
      }
      if (p->j->src[p->pos] != close)
        return 0;
      p->pos++;
      break;
    }
  }
  p->depth--;
  p->j->tok[t].end = p->pos;
  p->j->tok[t].next = p->j->n;
  return 1;
}

static int literal(Parser *p) {
  const char *s = p->j->src;
  u32 start = p->pos;
  u8 type;
  int t;
  while (p->pos < p->len) {
    char c = s[p->pos];
    if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
        c == '\n' || c == '\r')
      break;
    p->pos++;
  }
  if (p->pos == start)
    return 0;
  if (s[start] == 't')
    type = JSON_TRUE;
  else if (s[start] == 'f')
    type = JSON_FALSE;
  else if (s[start] == 'n')
    type = JSON_NULL;
  else
    type = JSON_NUM;
  t = add(p, type, start);
  if (t < 0)
    return 0;
  p->j->tok[t].end = p->pos;
  p->j->tok[t].next = p->j->n;
  return 1;
}

static int value(Parser *p) {
  skip_ws(p);
  if (p->pos >= p->len)
    return 0;
  switch (p->j->src[p->pos]) {
    case '{': return container(p, 1);
    case '[': return container(p, 0);
    case '"': return string(p);
    default:  return literal(p);
  }
}

int json_parse(Json *j, const char *src, u32 len, JsonTok *tok, u32 cap) {
  Parser p = {j, 0, len, 0};
  j->src = src;
  j->tok = tok;
  j->n = 0;
  j->cap = cap;
  return value(&p);
}

u32 json_count(const Json *j, int t) {
  return t < 0 ? 0 : j->tok[t].count;
}

int json_get(const Json *j, int obj, const char *key) {
  u32 i, n;
  if (obj < 0 || j->tok[obj].type != JSON_OBJ)
    return -1;
  i = (u32)obj + 1;
  for (n = 0; n < j->tok[obj].count; n++) {
    int k = (int)i;
    i = j->tok[k].next; /* the value */
    if (json_is(j, k, key))
      return (int)i;
    i = j->tok[i].next;
  }
  return -1;
}

int json_at(const Json *j, int arr, u32 idx) {
  u32 i;
  if (arr < 0 || j->tok[arr].type != JSON_ARR || idx >= j->tok[arr].count)
    return -1;
  i = (u32)arr + 1;
  while (idx--)
    i = j->tok[i].next;
  return (int)i;
}

int json_is(const Json *j, int t, const char *s) {
  u32 a;
  if (t < 0 || j->tok[t].type != JSON_STR)
    return 0;
  for (a = j->tok[t].start; a < j->tok[t].end; a++, s++)
    if (!*s || j->src[a] != *s)
      return 0;
  return *s == 0;
}

double json_num(const Json *j, int t, double def) {
  const char *s;
  u32 i, end;
  double v = 0.0, scale = 1.0;
  int neg = 0, eneg = 0, e = 0;

  if (t < 0 || j->tok[t].type != JSON_NUM)
    return def;
  s = j->src;
  i = j->tok[t].start;
  end = j->tok[t].end;
  if (i < end && (s[i] == '-' || s[i] == '+'))
    neg = s[i++] == '-';
  for (; i < end && s[i] >= '0' && s[i] <= '9'; i++)
    v = v * 10.0 + (s[i] - '0');
  if (i < end && s[i] == '.')
    for (i++; i < end && s[i] >= '0' && s[i] <= '9'; i++) {
      scale *= 0.1;
      v += (s[i] - '0') * scale;
    }
  if (i < end && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < end && (s[i] == '-' || s[i] == '+'))
      eneg = s[i++] == '-';
    for (; i < end && s[i] >= '0' && s[i] <= '9'; i++)
      if (e < 400)
        e = e * 10 + (s[i] - '0');
    while (e--)
      v = eneg ? v * 0.1 : v * 10.0;
  }
  return neg ? -v : v;
}

int json_int(const Json *j, int t, int def) {
  double v = json_num(j, t, (double)def);
  return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

int json_bool(const Json *j, int t, int def) {
  if (t < 0)
    return def;
  if (j->tok[t].type == JSON_TRUE)
    return 1;
  if (j->tok[t].type == JSON_FALSE)
    return 0;
  return def;
}

void json_str(const Json *j, int t, char *out, u32 max) {
  u32 a, o = 0;
  if (!max)
    return;
  if (t >= 0 && j->tok[t].type == JSON_STR)
    for (a = j->tok[t].start; a < j->tok[t].end && o + 1 < max; a++) {
      char c = j->src[a];
      if (c == '\\' && a + 1 < j->tok[t].end) {
        c = j->src[++a];
        if (c == 'n' || c == 't' || c == 'r')
          c = ' ';
        else if (c == 'u') {
          a += 4; /* \uXXXX */
          c = '?';
        }
      } else if ((unsigned char)c >= 0x80) {
        while (a + 1 < j->tok[t].end &&
               ((unsigned char)j->src[a + 1] & 0xC0) == 0x80)
          a++; /* the rest of one UTF-8 character */
        c = '?';
      }
      out[o++] = c;
    }
  out[o] = 0;
}
