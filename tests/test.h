// test.h - tiny assertion helpers for occ's self-checking test programs.
//
// Every test prints "OK" and exits with status 0 when all checks pass. The
// same programs are also compiled with a reference compiler (GCC/Clang) by
// `tests/run.sh --reference`, which validates the tests themselves.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failures;
static int test_checks;

static void test_check_int(long long expected, long long actual, const char *code, const char *file, int line) {
  test_checks++;
  if (expected == actual)
    return;
  printf("%s:%d: %s => expected %lld, got %lld\n", file, line, code, expected, actual);
  test_failures++;
}

static void test_check_str(const char *expected, const char *actual, const char *code, const char *file,
                           int line) {
  test_checks++;
  if (strcmp(expected, actual) == 0)
    return;
  printf("%s:%d: %s => expected \"%s\", got \"%s\"\n", file, line, code, expected, actual);
  test_failures++;
}

static void test_check_dbl(double expected, double actual, const char *code, const char *file, int line) {
  test_checks++;
  double diff = expected - actual;
  if (diff < 0)
    diff = -diff;
  if (diff <= 1e-9 * (expected < 0 ? -expected : expected) + 1e-12)
    return;
  printf("%s:%d: %s => expected %.17g, got %.17g\n", file, line, code, expected, actual);
  test_failures++;
}

#define ASSERT(expected, actual) test_check_int((long long)(expected), (long long)(actual), #actual, __FILE__, __LINE__)
#define ASSERT_STR(expected, actual) test_check_str((expected), (actual), #actual, __FILE__, __LINE__)
#define ASSERT_DBL(expected, actual) test_check_dbl((expected), (actual), #actual, __FILE__, __LINE__)

static int test_done(void) {
  if (test_failures) {
    printf("%d of %d checks FAILED\n", test_failures, test_checks);
    return 1;
  }
  printf("OK (%d checks)\n", test_checks);
  return 0;
}
