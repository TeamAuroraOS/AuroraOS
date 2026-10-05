#ifndef AURORA_JSON_H
#define AURORA_JSON_H

#include "aurora.h"

/* A JSON document as a flat list of tokens in document order. An object's
 * members are its key tokens, each followed by its value's tokens. Token
 * indices are ints; -1 means "not there". */

enum {
  JSON_OBJ = 1,
  JSON_ARR,
  JSON_STR,
  JSON_NUM,
  JSON_TRUE,
  JSON_FALSE,
  JSON_NULL,
};

typedef struct {
  u8 type;
  u32 start, end; /* the text, quotes excluded for a string */
  u32 next;       /* the token after this one's whole subtree */
  u32 count;      /* members of an object, elements of an array */
} JsonTok;

typedef struct {
  const char *src;
  JsonTok *tok;
  u32 n, cap;
} Json;

/* 1 on success. `tok` holds up to `cap` tokens; the document must fit. */
int json_parse(Json *j, const char *src, u32 len, JsonTok *tok, u32 cap);

int json_get(const Json *j, int obj, const char *key);
int json_at(const Json *j, int arr, u32 i);
u32 json_count(const Json *j, int t);

int json_int(const Json *j, int t, int def);
double json_num(const Json *j, int t, double def);
int json_bool(const Json *j, int t, int def);
/* 1 if `t` is the string `s`. */
int json_is(const Json *j, int t, const char *s);
/* The string, escapes resolved and anything outside ASCII as '?', cut to fit
 * `max` bytes with its terminator. */
void json_str(const Json *j, int t, char *out, u32 max);
/* The same for text to show: line breaks kept, and letters up to U+00FF (what
 * the UI fonts draw) as UTF-8. */
void json_text(const Json *j, int t, char *out, u32 max);

#endif
