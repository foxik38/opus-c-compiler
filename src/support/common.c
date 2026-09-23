// common.c - allocation helpers and small string utilities.
#include "support/common.h"

static void out_of_memory(void) {
  fputs("occ: fatal: out of memory\n", stderr);
  exit(1);
}

void *xmalloc(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p)
    out_of_memory();
  return p;
}

void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p)
    out_of_memory();
  return p;
}

void *xrealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size ? size : 1);
  if (!p)
    out_of_memory();
  return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *vformat(const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int len = vsnprintf(nullptr, 0, fmt, ap2);
  va_end(ap2);
  char *buf = xmalloc((size_t)len + 1);
  vsnprintf(buf, (size_t)len + 1, fmt, ap);
  return buf;
}

char *format(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = vformat(fmt, ap);
  va_end(ap);
  return s;
}

void panic(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("occ: internal compiler error: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputs("\nThis is a bug in occ, not in your program.\n", stderr);
  va_end(ap);
  exit(70);
}

bool starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool ends_with(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  return n >= m && strcmp(s + n - m, suffix) == 0;
}

int64_t align_to(int64_t n, int64_t align) { return (n + align - 1) / align * align; }

bool is_power_of_two(uint64_t n) { return n && !(n & (n - 1)); }

int log2_u64(uint64_t n) {
  int r = 0;
  while (n >>= 1)
    r++;
  return r;
}
