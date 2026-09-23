// The preprocessor: macros, stringizing, pasting, conditionals, includes.
#include "test.h"

#include "preproc_helper.h"
#include "preproc_helper.h" // include guard: harmless second inclusion

#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define VERSION 3
#define EMPTY
#define COUNT_ARGS(...) COUNT_ARGS_(__VA_ARGS__ __VA_OPT__(, ) 5, 4, 3, 2, 1, 0)
#define COUNT_ARGS_(a, b, c, d, e, n, ...) n
#define LOG(fmt, ...) snprintf(buf, sizeof buf, fmt __VA_OPT__(, ) __VA_ARGS__)
#define GNU_LOG(fmt, args...) snprintf(buf, sizeof buf, fmt, ##args)
#define RECURSIVE RECURSIVE + 1
#define f(x) (x + 1)
#define g f
#define OBJ (1 + 2)
#define APPLY(m, v) m(v)
#define LPAREN (
#define DEFER(x) x EMPTY

#if defined(VERSION) && VERSION >= 3 && !defined(UNDEFINED_THING)
static int version_ok = 1;
#else
static int version_ok = 0;
#endif

#if 0
#error "must be skipped"
#elif VERSION == 3
static int elif_taken = 1;
#else
static int elif_taken = 0;
#endif

#ifdef HELPER_VALUE
static int helper_seen = HELPER_VALUE;
#endif

#if (2 + 3) * 4 == 20 && (1 ? 2 : 3) == 2 && -1 < 0 && (0x10 >> 2) == 4 && 'A' == 65
static int arith_ok = 1;
#endif

#if __has_include(<stdio.h>) && !__has_include("does_not_exist.h")
static int has_include_ok = 1;
#endif

#undef VERSION
#ifndef VERSION
static int undef_ok = 1;
#endif

#line 1000
static int line_after = __LINE__;

int main(void) {
  char buf[64];
  ASSERT(49, SQUARE(7));
  ASSERT(9, SQUARE(1 + 2));
  ASSERT(5, MAX(3, 5));
  ASSERT_STR("hello world", STR(hello world));
  ASSERT_STR("a + b", STR(a + b));
  ASSERT_STR("\"quoted\"", STR("quoted"));
  ASSERT_STR("'\\n'", STR('\n'));
  ASSERT_STR("3", XSTR(3));
  ASSERT_STR("OBJ", STR(OBJ));
  ASSERT_STR("(1 + 2)", XSTR(OBJ));
  int CAT(my, var) = 11;
  ASSERT(11, myvar);
  ASSERT(12, XCAT(my, var) + 1);
  ASSERT(0, COUNT_ARGS());
  ASSERT(1, COUNT_ARGS(a));
  ASSERT(3, COUNT_ARGS(a, b, c));
  LOG("plain");
  ASSERT_STR("plain", buf);
  LOG("%d-%d", 1, 2);
  ASSERT_STR("1-2", buf);
  GNU_LOG("gnu");
  ASSERT_STR("gnu", buf);
  GNU_LOG("gnu %s", "args");
  ASSERT_STR("gnu args", buf);
  int value_of_recursive = 5;
#define value_of_recursive value_of_recursive + 1 // self-reference expands once
  ASSERT(6, value_of_recursive);
#undef value_of_recursive
  ASSERT(3, g(2));
  ASSERT(3, APPLY(f, 2));
  ASSERT(1, version_ok);
  ASSERT(1, elif_taken);
  ASSERT(42, helper_seen);
  ASSERT(1, arith_ok);
  ASSERT(1, has_include_ok);
  ASSERT(1, undef_ok);
  ASSERT(1000, line_after);
  ASSERT(8, HELPER_TWICE(4));
  ASSERT(1, strstr(__FILE__, "preproc.c") != nullptr);
  ASSERT(1, __STDC__);
  ASSERT(1, __STDC_HOSTED__);
  ASSERT(1, __x86_64__);
  ASSERT(8, __SIZEOF_POINTER__);
  ASSERT(0, __COUNTER__);
  ASSERT(1, __COUNTER__);
  ASSERT(11, (int)strlen(__DATE__));
  ASSERT(8, (int)strlen(__TIME__));
  return test_done();
}
