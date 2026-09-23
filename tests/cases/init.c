// Initializers: designators, brace elision, strings, static data with relocations.
#include "test.h"

struct S {
  int a;
  char b[4];
  struct {
    short x, y;
  } inner;
  double d;
};

union U {
  int i;
  char c[8];
  double d;
};

// Static storage: zero-initialized, constant data and address constants.
static int zeros[100];
static int primes[] = {2, 3, 5, 7, 11, 13};
static char greeting[] = "hello";
static const char *messages[] = {"zero", "one", "two"};
static int *prime_ptr = &primes[3];
static const char *msg_ptr = "static string";
static struct S gs = {1, "ab", {3, 4}, 5.5};
static struct S gdesig = {.d = 2.5, .inner.y = 9, .a = 7};
static int garr2[3][2] = {{1, 2}, {3, 4}, {5, 6}};
static int gelide[2][2] = {1, 2, 3, 4};
static union U gu = {.d = 1.5};
static char *gchars = (char[]){'x', 'y', 0};
static long gdiff = (long)(&primes[5] - &primes[1]);
static int tentative;
static double gpi = 3.14159;
static float gf = -2.5f;
static _Bool gb = 42;
static unsigned long gmax = 0xffffffffffffffffUL;
static struct {
  int *p;
  int n;
} gtable[] = {{&primes[0], 1}, {&primes[2], 2}, {nullptr, 0}};

int counter(void) {
  static int count = 100;
  return count++;
}

int main(void) {
  ASSERT(0, zeros[0] + zeros[99]);
  ASSERT(6, sizeof primes / sizeof primes[0]);
  ASSERT(13, primes[5]);
  ASSERT(6, sizeof greeting);
  ASSERT_STR("hello", greeting);
  ASSERT_STR("two", messages[2]);
  ASSERT(7, *prime_ptr);
  ASSERT_STR("static string", msg_ptr);
  ASSERT(1, gs.a);
  ASSERT_STR("ab", gs.b);
  ASSERT(4, gs.inner.y);
  ASSERT_DBL(5.5, gs.d);
  ASSERT(7, gdesig.a);
  ASSERT(0, gdesig.inner.x);
  ASSERT(9, gdesig.inner.y);
  ASSERT_DBL(2.5, gdesig.d);
  ASSERT(6, garr2[2][1]);
  ASSERT(3, gelide[1][0]);
  ASSERT_DBL(1.5, gu.d);
  ASSERT_STR("xy", gchars);
  ASSERT(4, gdiff);
  ASSERT(0, tentative);
  ASSERT_DBL(3.14159, gpi);
  ASSERT_DBL(-2.5, gf);
  ASSERT(1, gb);
  ASSERT(-1, (long)gmax);
  ASSERT(5, gtable[1].p[0]);
  ASSERT(2, gtable[1].n);
  ASSERT(1, gtable[2].p == nullptr);

  // Static locals keep their value.
  ASSERT(100, counter());
  ASSERT(101, counter());
  ASSERT(102, counter());

  // Automatic storage.
  int a[5] = {1, 2};
  ASSERT(3, a[0] + a[1] + a[2] + a[3] + a[4]);
  int b[] = {[3] = 4, [1] = 2, 3};
  ASSERT(4, sizeof b / sizeof b[0]);
  ASSERT(3, b[2]);
  ASSERT(0, b[0]);
  int c[10] = {[2 ... 5] = 7};
  ASSERT(28, c[0] + c[2] + c[3] + c[4] + c[5] + c[6]);
  char s1[] = "abc";
  ASSERT(4, sizeof s1);
  char s2[10] = "abc";
  ASSERT(0, s2[9]);
  ASSERT('c', s2[2]);
  char s3[3] = "abc"; // no terminator: allowed in C
  ASSERT('c', s3[2]);
  char s4[] = {"braced"};
  ASSERT(7, sizeof s4);
  struct S ls = {10, {'x'}, {.y = 2}, 1.25};
  ASSERT(10, ls.a);
  ASSERT('x', ls.b[0]);
  ASSERT(0, ls.b[1]);
  ASSERT(0, ls.inner.x);
  ASSERT(2, ls.inner.y);
  struct S empty = {};
  ASSERT(0, empty.a + empty.inner.x);
  ASSERT_DBL(0.0, empty.d);
  struct S copy = ls;
  ASSERT(10, copy.a);
  union U lu = {.c = "abcdefg"};
  ASSERT('g', lu.c[6]);
  union U lu2 = {65};
  ASSERT(65, lu2.i);
  int m[2][3] = {1, 2, 3, 4, 5, 6};
  ASSERT(6, m[1][2]);
  int m2[][2] = {{1}, {2, 3}, {4}};
  ASSERT(3, sizeof m2 / sizeof m2[0]);
  ASSERT(0, m2[2][1]);
  struct S arr[2] = {{1, "a", {1, 1}, 1}, [1].inner.x = 5};
  ASSERT(5, arr[1].inner.x);
  ASSERT(0, arr[1].a);
  int scalar = {42};
  ASSERT(42, scalar);
  int zero = {};
  ASSERT(0, zero);
  const char *strs[] = {"a", "bb", "ccc"};
  ASSERT(3, (int)strlen(strs[2]));
  wchar_t_like: {
    int wide[] = {L'a', L'b'};
    ASSERT('b', wide[1]);
  }

  // Initializer expressions may refer to earlier variables.
  int x = 5, y = x * 2, z = x + y;
  ASSERT(15, z);

  // Large local arrays are zero-filled.
  int big[1000] = {[999] = 1};
  int sum = 0;
  for (int i = 0; i < 1000; i++)
    sum += big[i];
  ASSERT(1, sum);

  return test_done();
}
