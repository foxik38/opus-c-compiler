// strbuf.c - growable string buffer.
#include "support/strbuf.h"

static void sb_grow(StrBuf *sb, size_t extra) {
  size_t need = sb->len + extra + 1;
  if (need <= sb->cap)
    return;
  size_t cap = sb->cap ? sb->cap : 64;
  while (cap < need)
    cap *= 2;
  sb->data = xrealloc(sb->data, cap);
  sb->cap = cap;
}

void sb_append(StrBuf *sb, const char *s, size_t n) {
  sb_grow(sb, n);
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }

void sb_putc(StrBuf *sb, char c) { sb_append(sb, &c, 1); }

void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap2);
  va_end(ap2);
  sb_grow(sb, (size_t)n);
  vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
  sb->len += (size_t)n;
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sb_vprintf(sb, fmt, ap);
  va_end(ap);
}

void sb_clear(StrBuf *sb) {
  sb->len = 0;
  if (sb->data)
    sb->data[0] = '\0';
}

const char *sb_str(const StrBuf *sb) { return sb->data ? sb->data : ""; }
