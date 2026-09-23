// common.h - project-wide includes, allocation helpers and small utilities.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCC_VERSION "1.0.0"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NEW(T) ((T *)xcalloc(1, sizeof(T)))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// The compiler is a short-lived batch process: memory is allocated from the
// heap and intentionally never freed. Allocation failure is fatal.
[[nodiscard]] void *xmalloc(size_t size);
[[nodiscard]] void *xcalloc(size_t n, size_t size);
[[nodiscard]] void *xrealloc(void *ptr, size_t size);
[[nodiscard]] char *xstrdup(const char *s);
[[nodiscard]] char *xstrndup(const char *s, size_t n);

// Returns a freshly allocated printf-formatted string.
[[nodiscard, gnu::format(printf, 1, 2)]] char *format(const char *fmt, ...);
[[nodiscard]] char *vformat(const char *fmt, va_list ap);

// Internal compiler error: a bug in occ, not in the user's program.
[[noreturn, gnu::format(printf, 1, 2)]] void panic(const char *fmt, ...);

#define ICE(...) panic(__VA_ARGS__)
#define ICE_UNREACHABLE() panic("unreachable code reached at %s:%d", __FILE__, __LINE__)

bool starts_with(const char *s, const char *prefix);
bool locale_is_utf8(void); // LC_ALL / LC_CTYPE / LANG name a UTF-8 locale
bool ends_with(const char *s, const char *suffix);
int64_t align_to(int64_t n, int64_t align);
bool is_power_of_two(uint64_t n);
int log2_u64(uint64_t n);
