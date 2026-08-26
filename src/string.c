/*
 * Freestanding libc string/memory helpers.
 *
 * AuroraOS builds with -ffreestanding -nostdlib, so newlib's libc is not
 * linked. FatFs (ff.c) uses memcpy/memset/memcmp/strchr/strlen, and GCC itself
 * may emit calls to memcpy/memset for aggregate copies, so we provide our own.
 * Signatures match <string.h>. -fno-builtin (and -O0) keep the compiler from
 * turning these loops back into calls to themselves.
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  while (n--)
    *d++ = (unsigned char)c;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (d == s || n == 0)
    return dst;
  if (d < s) {
    while (n--)
      *d++ = *s++;
  } else { /* overlapping, copy backwards */
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *pa = (const unsigned char *)a;
  const unsigned char *pb = (const unsigned char *)b;
  while (n--) {
    if (*pa != *pb)
      return (int)*pa - (int)*pb;
    pa++;
    pb++;
  }
  return 0;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (*s == '\0')
      return NULL;
  }
}
