// vec.h - a minimal type-generic growable array.
//
//   typedef VEC(int) IntVec;
//   IntVec v = {};
//   vec_push(&v, 42);
#pragma once

#include "support/common.h"

#define VEC(T)                                                                                     \
  struct {                                                                                         \
    T *data;                                                                                       \
    size_t len;                                                                                    \
    size_t cap;                                                                                    \
  }

#define vec_reserve(v, n)                                                                          \
  do {                                                                                             \
    if ((v)->cap < (n)) {                                                                          \
      (v)->cap = (v)->cap ? (v)->cap : 8;                                                          \
      while ((v)->cap < (n))                                                                       \
        (v)->cap *= 2;                                                                             \
      (v)->data = xrealloc((v)->data, (v)->cap * sizeof *(v)->data);                               \
    }                                                                                              \
  } while (0)

#define vec_push(v, x)                                                                             \
  do {                                                                                             \
    vec_reserve((v), (v)->len + 1);                                                                \
    (v)->data[(v)->len++] = (x);                                                                   \
  } while (0)

#define vec_last(v) ((v)->data[(v)->len - 1])

typedef VEC(char *) StrVec;
