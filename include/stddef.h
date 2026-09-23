/* stddef.h - common definitions (C23 7.21), provided by occ. */
#ifndef __OCC_STDDEF_H
#define __OCC_STDDEF_H

#define __STDC_VERSION_STDDEF_H__ 202311L

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef int wchar_t;
typedef typeof(nullptr) nullptr_t;

typedef struct {
  alignas(16) long long __max_align_ll;
  alignas(16) double __max_align_d;
} max_align_t;

#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
#define unreachable() __builtin_unreachable()

#endif
