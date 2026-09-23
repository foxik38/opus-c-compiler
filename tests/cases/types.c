// The type system: sizes, alignment, _Generic, typedefs, qualifiers, enums.
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "test.h"

#define TYPE_NAME(x)                                                                                           \
  _Generic((x),                                                                                                \
      _Bool: "bool",                                                                                           \
      char: "char",                                                                                            \
      signed char: "signed char",                                                                              \
      unsigned char: "unsigned char",                                                                          \
      short: "short",                                                                                          \
      unsigned short: "unsigned short",                                                                        \
      int: "int",                                                                                              \
      unsigned: "unsigned int",                                                                                \
      long: "long",                                                                                            \
      unsigned long: "unsigned long",                                                                          \
      long long: "long long",                                                                                  \
      unsigned long long: "unsigned long long",                                                                \
      float: "float",                                                                                          \
      double: "double",                                                                                        \
      char *: "char *",                                                                                        \
      const char *: "const char *",                                                                            \
      int *: "int *",                                                                                          \
      default: "other")

typedef unsigned long ulong;
typedef int (*Callback)(int);
typedef struct Vec Vec;
struct Vec {
  int x, y;
};
typedef int Arr3[3];

enum Color { RED, GREEN = 10, BLUE, ALPHA = -1 };
enum { ANON_A = 100, ANON_B };

struct Align {
  char c;
  alignas(16) int x;
};

int main(void) {
  ASSERT(1, sizeof(char));
  ASSERT(2, sizeof(short));
  ASSERT(4, sizeof(int));
  ASSERT(8, sizeof(long));
  ASSERT(8, sizeof(long long));
  ASSERT(8, sizeof(void *));
  ASSERT(8, sizeof(size_t));
  ASSERT(4, sizeof(enum Color));
  ASSERT(1, alignof(char));
  ASSERT(8, alignof(double));
  ASSERT(8, alignof(long));
  ASSERT(16, alignof(struct Align));
  ASSERT(16, offsetof(struct Align, x));
  ASSERT(32, sizeof(struct Align));
  alignas(32) char aligned_buf[3];
  ASSERT(0, (uintptr_t)aligned_buf % 16);

  // Usual arithmetic conversions, seen through _Generic.
  char c = 1;
  short s = 1;
  unsigned u = 1;
  long l = 1;
  ASSERT_STR("int", TYPE_NAME(c + c));
  ASSERT_STR("int", TYPE_NAME(s * s));
  ASSERT_STR("unsigned int", TYPE_NAME(u + 1));
  ASSERT_STR("long", TYPE_NAME(u + l));
  ASSERT_STR("unsigned long", TYPE_NAME(1ul + 1));
  ASSERT_STR("long long", TYPE_NAME(1ll));
  ASSERT_STR("double", TYPE_NAME(1.0f + 1.0));
  ASSERT_STR("float", TYPE_NAME(1.0f + 1));
  ASSERT_STR("char", TYPE_NAME(c));
  ASSERT_STR("int", TYPE_NAME('a'));
  ASSERT_STR("bool", TYPE_NAME(true));
  ASSERT_STR("int", TYPE_NAME(1 == 1));
  ASSERT_STR("char *", TYPE_NAME((char *)0));
  ASSERT_STR("char *", TYPE_NAME("literal" + 0)); // literals are char[N] in C
  ASSERT_STR("const char *", TYPE_NAME((const char *)"literal"));
  ASSERT_STR("int *", TYPE_NAME(&(int){0}));
  ASSERT_STR("unsigned long", TYPE_NAME(sizeof(int)));
  ASSERT_STR("long", TYPE_NAME((ptrdiff_t)0));
  ASSERT_STR("int", TYPE_NAME(RED));
  ASSERT_STR("other", TYPE_NAME((void *)0));

  // Integer constant types.
  ASSERT_STR("int", TYPE_NAME(2147483647));
  ASSERT_STR("long", TYPE_NAME(2147483648));
  ASSERT_STR("unsigned int", TYPE_NAME(0xffffffff));
  ASSERT_STR("long", TYPE_NAME(0x100000000));
  ASSERT_STR("unsigned long", TYPE_NAME(0xffffffffffffffff));
  ASSERT_STR("unsigned int", TYPE_NAME(10u));
  ASSERT_STR("unsigned long", TYPE_NAME(10lu));

  // Typedefs.
  ulong ul = 5;
  ASSERT(8, sizeof ul);
  Vec v = {1, 2};
  ASSERT(3, v.x + v.y);
  Arr3 a3 = {1, 2, 3};
  ASSERT(12, sizeof a3);
  Callback cb = nullptr;
  ASSERT(1, cb == nullptr);

  // Enums.
  ASSERT(0, RED);
  ASSERT(11, BLUE);
  ASSERT(-1, ALPHA);
  ASSERT(101, ANON_B);
  enum Color col = GREEN;
  col++;
  ASSERT(BLUE, col);

  // Qualifiers.
  const int ci = 3;
  volatile int vi = 4;
  vi += ci;
  ASSERT(7, vi);
  const char *pc = "abc";
  pc++;
  ASSERT('b', *pc);
  char *const cp = (char[]){'x', 'y'};
  cp[1] = 'z';
  ASSERT('z', cp[1]);
  int *restrict rp = &vi;
  ASSERT(7, *rp);

  // Casts between pointers and integers.
  int obj = 5;
  uintptr_t addr = (uintptr_t)&obj;
  ASSERT(5, *(int *)addr);
  intptr_t iaddr = (intptr_t)&obj;
  ASSERT(1, iaddr == (intptr_t)addr);

  // Limits from <stdint.h>.
  ASSERT(127, INT8_MAX);
  ASSERT(-32768, INT16_MIN);
  ASSERT(4294967295u, UINT32_MAX);
  int8_t i8 = -128;
  ASSERT(-128, i8);
  uint16_t u16 = 65535;
  u16++;
  ASSERT(0, u16);
  int64_t i64 = INT64_MIN;
  ASSERT(1, i64 < 0);

  return test_done();
}
