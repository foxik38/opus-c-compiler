// Functions: recursion, many arguments, variadics and function pointers.
#include <stdarg.h>

#include "test.h"

static int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int ackermann(int m, int n) {
  if (m == 0)
    return n + 1;
  if (n == 0)
    return ackermann(m - 1, 1);
  return ackermann(m - 1, ackermann(m, n - 1));
}

static long many_ints(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
  return a + 2L * b + 3L * c + 4L * d + 5L * e + 6L * f + 7L * g + 8L * h + 9L * i + 10L * j;
}

static double many_doubles(double a, double b, double c, double d, double e, double f, double g, double h,
                           double i, double j) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}

static double mixed_args(int a, double b, char c, float d, long e, double f, short g, int h, int i, int j, float k,
                         double l, double m, double n, double o, double p, int q) {
  return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q;
}

static int sum_ints(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int);
  va_end(ap);
  return s;
}

static double sum_mixed(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  double s = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p == 'i')
      s += va_arg(ap, int);
    else if (*p == 'l')
      s += (double)va_arg(ap, long);
    else if (*p == 'd')
      s += va_arg(ap, double);
    else if (*p == 's')
      s += (double)strlen(va_arg(ap, char *));
  }
  va_end(ap);
  return s;
}

typedef struct {
  long a, b;
} Pair;

typedef struct {
  long v[4];
} Quad;

static long sum_structs(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long s = 0;
  for (int i = 0; i < n; i++) {
    Pair p = va_arg(ap, Pair);
    Quad q = va_arg(ap, Quad);
    s += p.a + p.b + q.v[0] + q.v[3];
  }
  va_end(ap);
  return s;
}

static int vsum(int n, va_list ap) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int);
  return s;
}

static int sum_via_va_list(int n, ...) {
  va_list ap, copy;
  va_start(ap, n);
  va_copy(copy, ap);
  int a = vsum(n, ap);
  int b = vsum(n, copy);
  va_end(copy);
  va_end(ap);
  return a == b ? a : -1;
}

static char *format_str(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return buf;
}

static int counter;
static void bump(void) { counter++; }
static void call_n(void (*f)(void), int n) {
  for (int i = 0; i < n; i++)
    f();
}

static int compare_ints(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

static char ret_char(int x) { return (char)x; }
static unsigned char ret_uchar(int x) { return (unsigned char)x; }
static short ret_short(int x) { return (short)x; }
static _Bool ret_bool(int x) { return x; }
static int take_char(char c) { return c; }
static int take_short(short s) { return s; }

static int (*pick(int which))(int) {
  return which ? fact : fib;
}

// Declarations after use via prototypes and unnamed parameters (C23).
static int proto(int, int);
static int unnamed(int a, int) { return a; }

int main(void) {
  ASSERT(120, fact(5));
  ASSERT(3628800, fact(10));
  ASSERT(6765, fib(20));
  ASSERT(61, ackermann(3, 3));
  ASSERT(55 + 0L, many_ints(1, 1, 1, 1, 1, 1, 1, 1, 1, 1));
  ASSERT(385, many_ints(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
  ASSERT_DBL(385.0, many_doubles(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
  ASSERT_DBL(1 + 2.5 + 3 + 4.5 + 5 + 6.5 + 7 + 8 + 9 + 10 + 11.5 + 12 + 13 + 14 + 15 + 16 + 17,
             mixed_args(1, 2.5, 3, 4.5f, 5, 6.5, 7, 8, 9, 10, 11.5f, 12, 13, 14, 15, 16, 17));

  ASSERT(0, sum_ints(0));
  ASSERT(15, sum_ints(5, 1, 2, 3, 4, 5));
  ASSERT(78, sum_ints(12, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));
  ASSERT_DBL(1 + 2 + 3.5 + 4 + 5.25 + 3, sum_mixed("iidldsx", 1, 2, 3.5, 4L, 5.25, "abc"));
  ASSERT_DBL(45.0, sum_mixed("dddddddddd", 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0));
  Pair p1 = {1, 2};
  Quad q1 = {{3, 0, 0, 4}};
  ASSERT(20, sum_structs(2, p1, q1, p1, q1));
  ASSERT(10, sum_via_va_list(4, 1, 2, 3, 4));
  char buf[64];
  ASSERT_STR("x=42 y=hi z=1.50", format_str(buf, sizeof buf, "x=%d y=%s z=%.2f", 42, "hi", 1.5));

  call_n(bump, 7);
  ASSERT(7, counter);

  int arr[] = {5, 3, 9, 1, 7};
  qsort(arr, 5, sizeof(int), compare_ints);
  ASSERT(1, arr[0]);
  ASSERT(9, arr[4]);

  ASSERT(-1, ret_char(255));
  ASSERT(255, ret_uchar(-1));
  ASSERT(-1, ret_short(65535));
  ASSERT(1, ret_bool(5));
  ASSERT(0, ret_bool(0));
  ASSERT(-128, take_char(128));
  ASSERT(-1, take_short(0xffff));

  ASSERT(24, pick(1)(4));
  ASSERT(3, pick(0)(4));
  ASSERT(7, proto(3, 4));
  ASSERT(9, unnamed(9, 1));

  return test_done();
}

static int proto(int a, int b) { return a + b; }
