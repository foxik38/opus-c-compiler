// strbuf.h - growable, always NUL-terminated string buffer.
#pragma once

#include "support/common.h"

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

void sb_append(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_putc(StrBuf *sb, char c);
[[gnu::format(printf, 2, 3)]] void sb_printf(StrBuf *sb, const char *fmt, ...);
void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap);
void sb_clear(StrBuf *sb);
// Returns the buffer contents ("" when empty). The buffer keeps ownership.
const char *sb_str(const StrBuf *sb);
