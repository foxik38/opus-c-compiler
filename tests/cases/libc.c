// Interoperability with the C library (glibc): the ABI must match exactly.
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "test.h"

static jmp_buf env;

static void jump_back(int v) { longjmp(env, v); }

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

struct Item {
  int key;
  const char *name;
};

static int cmp_item(const void *a, const void *b) {
  return ((const struct Item *)a)->key - ((const struct Item *)b)->key;
}

int main(int argc, char **argv) {
  // argc/argv come from the C runtime.
  ASSERT(3, argc);
  ASSERT_STR("arg1", argv[1]);
  ASSERT_STR("arg2", argv[2]);
  ASSERT(1, argv[3] == nullptr);

  // Dynamic memory.
  int *v = malloc(100 * sizeof *v);
  for (int i = 0; i < 100; i++)
    v[i] = i;
  v = realloc(v, 200 * sizeof *v);
  ASSERT(99, v[99]);
  free(v);
  char *z = calloc(16, 1);
  ASSERT(0, z[15]);
  free(z);

  // Sorting and searching with callbacks.
  const char *words[] = {"pear", "apple", "fig", "kiwi"};
  qsort(words, 4, sizeof words[0], cmp_str);
  ASSERT_STR("apple", words[0]);
  ASSERT_STR("pear", words[3]);
  struct Item items[] = {{5, "five"}, {1, "one"}, {3, "three"}};
  qsort(items, 3, sizeof items[0], cmp_item);
  ASSERT_STR("one", items[0].name);
  struct Item key = {3, nullptr};
  struct Item *found = bsearch(&key, items, 3, sizeof items[0], cmp_item);
  ASSERT_STR("three", found->name);

  // Conversions and errno.
  ASSERT(-1234, atoi("-1234"));
  ASSERT(255, strtol("ff", nullptr, 16));
  ASSERT_DBL(2.5, strtod("2.5", nullptr));
  errno = 0;
  long big = strtol("99999999999999999999", nullptr, 10);
  ASSERT(LONG_MAX, big);
  ASSERT(ERANGE, errno);
  ASSERT(5, labs(-5));
  div_t dv = div(17, 5); // struct returned in registers by libc
  ASSERT(3, dv.quot);
  ASSERT(2, dv.rem);
  ldiv_t ldv = ldiv(-17, 5);
  ASSERT(-3, ldv.quot);
  ASSERT(-2, ldv.rem);

  // setjmp / longjmp.
  volatile int jumps = 0;
  int r = setjmp(env);
  if (r < 3) {
    jumps++;
    jump_back(r + 1);
  }
  ASSERT(3, r);
  ASSERT(3, jumps);

  // stdio to memory.
  char buf[128];
  int n = snprintf(buf, sizeof buf, "%5d|%-5s|%+.2e|%lu|%lld|%p", 42, "ab", 1234.5, (unsigned long)ULONG_MAX,
                   (long long)LLONG_MIN, (void *)0);
  ASSERT(1, n > 0);
  ASSERT_STR("   42|ab   |+1.23e+03|18446744073709551615|-9223372036854775808|(nil)", buf);
  FILE *fp = tmpfile();
  fprintf(fp, "line %d\n", 7);
  rewind(fp);
  int got = 0;
  ASSERT(1, fscanf(fp, "line %d", &got));
  ASSERT(7, got);
  fclose(fp);

  // Time and the environment.
  time_t now = time(nullptr);
  ASSERT(1, now > 1600000000);
  struct tm tm = {};
  tm.tm_year = 124;
  tm.tm_mday = 1;
  char date[32];
  strftime(date, sizeof date, "%Y-%m-%d", &tm);
  ASSERT_STR("2024-01-01", date);
  ASSERT(1, getenv("PATH") != nullptr);

  // Global streams from a shared library (accessed through the GOT).
  ASSERT(1, stdout != nullptr);
  ASSERT(0, ferror(stderr));

  return test_done();
}
