// Control flow: if, loops, switch, goto, break/continue.
#include "test.h"

static int classify(int x) {
  switch (x) {
  case 0:
    return 100;
  case 1:
  case 2:
    return 200;
  case 3 ... 7: // GNU case range
    return 300;
  case -5:
    return -500;
  default:
    return -1;
  }
}

// Dense switch (jump table at -o prod).
static const char *day(int d) {
  switch (d) {
  case 0: return "sun";
  case 1: return "mon";
  case 2: return "tue";
  case 3: return "wed";
  case 4: return "thu";
  case 5: return "fri";
  case 6: return "sat";
  }
  return "?";
}

static int fallthrough(int x) {
  int r = 0;
  switch (x) {
  case 1:
    r += 1;
    [[fallthrough]];
  case 2:
    r += 10;
    [[fallthrough]];
  case 3:
    r += 100;
    break;
  case 4:
    r = 4;
  }
  return r;
}

static long switch_long(long v) {
  switch (v) {
  case 0x100000000L: return 1;
  case -0x100000000L: return 2;
  case 5: return 3;
  default: return 0;
  }
}

static int sum_to(int n) {
  int s = 0;
  for (int i = 1; i <= n; i++)
    s += i;
  return s;
}

static int count_down(int n) {
  int steps = 0;
  while (n > 0) {
    n--;
    steps++;
  }
  return steps;
}

static int goto_loop(int n) {
  int i = 0;
again:
  if (i < n) {
    i++;
    goto again;
  }
  return i;
}

// A loop whose invariant address computation is hoisted (-o prod): the
// statement after it must still run exactly once (found by tests/fuzz).
// No return follows, so a duplicated statement would not be dead code.
static void after_hoisted_loop(long *a, long k, int *after) {
  for (int i = 0; i < 3; i++)
    a[k + 1] += i;
  ++*after;
}

int main(void) {
  // if / else chains.
  int x = 5, r;
  if (x > 3)
    r = 1;
  else
    r = 2;
  ASSERT(1, r);
  if (x < 3)
    r = 1;
  else if (x < 6)
    r = 2;
  else
    r = 3;
  ASSERT(2, r);
  if (0) {
    r = 99;
  }
  ASSERT(2, r);

  // Loops.
  ASSERT(55, sum_to(10));
  ASSERT(0, sum_to(0));
  ASSERT(7, count_down(7));
  ASSERT(4, goto_loop(4));

  int n = 0;
  do {
    n++;
  } while (n < 10);
  ASSERT(10, n);
  do
    n++;
  while (0);
  ASSERT(11, n);

  // break / continue.
  int s = 0;
  for (int i = 0; i < 100; i++) {
    if (i % 2)
      continue;
    if (i > 10)
      break;
    s += i;
  }
  ASSERT(30, s);

  int k = 0;
  for (;;) {
    if (++k == 42)
      break;
  }
  ASSERT(42, k);

  // Nested loops with labels.
  int found = -1;
  for (int i = 0; i < 10; i++)
    for (int j = 0; j < 10; j++)
      if (i * j == 42) {
        found = i * 10 + j;
        goto done;
      }
done:
  ASSERT(67, found);

  // Continue in while and do.
  int w = 0, odd = 0;
  while (w < 10) {
    w++;
    if (w % 2 == 0)
      continue;
    odd++;
  }
  ASSERT(5, odd);
  int d = 0, evens = 0;
  do {
    d++;
    if (d % 2)
      continue;
    evens++;
  } while (d < 10);
  ASSERT(5, evens);

  // switch.
  ASSERT(100, classify(0));
  ASSERT(200, classify(1));
  ASSERT(200, classify(2));
  ASSERT(300, classify(3));
  ASSERT(300, classify(7));
  ASSERT(-1, classify(8));
  ASSERT(-500, classify(-5));
  ASSERT(-1, classify(-4));
  ASSERT_STR("sun", day(0));
  ASSERT_STR("wed", day(3));
  ASSERT_STR("sat", day(6));
  ASSERT_STR("?", day(7));
  ASSERT_STR("?", day(-1));
  ASSERT(111, fallthrough(1));
  ASSERT(110, fallthrough(2));
  ASSERT(100, fallthrough(3));
  ASSERT(4, fallthrough(4));
  ASSERT(0, fallthrough(5));
  ASSERT(1, switch_long(0x100000000L));
  ASSERT(2, switch_long(-0x100000000L));
  ASSERT(3, switch_long(5));
  ASSERT(0, switch_long(6));

  // switch with break inside a loop.
  int hits = 0;
  for (int i = 0; i < 5; i++) {
    switch (i) {
    case 1:
      continue;
    case 3:
      break;
    default:
      hits++;
    }
  }
  ASSERT(3, hits);

  // Switch on char and unsigned values.
  unsigned char ch = 200;
  switch (ch) {
  case 200: r = 1; break;
  default: r = 0;
  }
  ASSERT(1, r);
  unsigned big = 4000000000u;
  switch (big) {
  case 4000000000u: r = 7; break;
  default: r = 0;
  }
  ASSERT(7, r);

  // Conditions of different types.
  double dz = 0.0;
  char *p = nullptr;
  ASSERT(0, dz ? 1 : 0);
  ASSERT(0, p ? 1 : 0);
  ASSERT(1, !p);
  long lz = 0x100000000;
  ASSERT(1, lz ? 1 : 0); // must test all 64 bits
  if (lz) r = 5; else r = 6;
  ASSERT(5, r);

  long hv[4] = {};
  int after = 0;
  after_hoisted_loop(hv, 1, &after);
  ASSERT(1, after);
  ASSERT(3, hv[2]);

  return test_done();
}
