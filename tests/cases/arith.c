// Integer arithmetic, operators, promotions and conversions.
#include <limits.h>
#include <stdint.h>

#include "test.h"

static int add(int a, int b) { return a + b; }

int main(void) {
  // Precedence and associativity.
  ASSERT(7, 1 + 2 * 3);
  ASSERT(9, (1 + 2) * 3);
  ASSERT(-1, 1 - 2);
  ASSERT(2, 10 - 5 - 3);
  ASSERT(5, 20 / 2 / 2);
  ASSERT(1, 5 % 2 * 1);
  ASSERT(10, - -10);
  ASSERT(-10, -+10);
  ASSERT(1, 1 + 2 == 3);
  ASSERT(1, 1 < 2 == 1);

  // Division and remainder truncate toward zero.
  ASSERT(-3, -7 / 2);
  ASSERT(-1, -7 % 2);
  ASSERT(3, 7 / 2);
  ASSERT(1, 7 % -2);
  ASSERT(-3, 7 / -2);
  int a = 17, b = 5;
  ASSERT(3, a / b);
  ASSERT(2, a % b);
  ASSERT(-3, -a / b);
  ASSERT(-2, -a % b);

  // Unsigned arithmetic wraps.
  unsigned u = 0;
  u--;
  ASSERT(UINT_MAX, u);
  ASSERT(0, u + 1);
  ASSERT(2147483647u, u / 2);
  unsigned char uc = 255;
  uc++;
  ASSERT(0, uc);
  signed char sc = 127;
  sc = (signed char)(sc + 1);
  ASSERT(-128, sc);

  // Shifts.
  ASSERT(8, 1 << 3);
  ASSERT(1, 8 >> 3);
  ASSERT(-1, -1 >> 1);
  ASSERT(-4, -16 >> 2);
  ASSERT(0x7fffffff, 0xffffffffu >> 1);
  ASSERT(1LL << 40, 1LL << 40);
  int sh = 5;
  ASSERT(32, 1 << sh);
  ASSERT(-2, -64 >> sh);
  ASSERT(0x0800000000000000, 0x8000000000000000u >> 4);

  // Bitwise.
  ASSERT(2, 6 & 3);
  ASSERT(7, 6 | 3);
  ASSERT(5, 6 ^ 3);
  ASSERT(-1, ~0);
  ASSERT(0xfffffff0, ~0xfu);

  // Comparisons and logic.
  ASSERT(1, 1 < 2);
  ASSERT(0, 2 < 1);
  ASSERT(1, 2 <= 2);
  ASSERT(1, 3 >= 2);
  ASSERT(0, 3 > 3);
  ASSERT(1, 3 != 2);
  ASSERT(1, !0);
  ASSERT(0, !5);
  ASSERT(1, 1 && 2);
  ASSERT(0, 1 && 0);
  ASSERT(1, 0 || 3);
  ASSERT(0, 0 || 0);
  ASSERT(0, -1 < 0u); // -1 converts to UINT_MAX
  ASSERT(1, -1 < 0L);

  // Short-circuit evaluation.
  int calls = 0;
  (void)(0 && (calls = 1));
  (void)(1 || (calls = 2));
  ASSERT(0, calls);
  (void)(1 && (calls = 3));
  ASSERT(3, calls);

  // Ternary and comma.
  ASSERT(5, 1 ? 5 : 6);
  ASSERT(6, 0 ? 5 : 6);
  ASSERT(3, (1, 2, 3));
  int t = 0;
  ASSERT(10, (t = 4, t + 6));

  // Compound assignment.
  int x = 10;
  x += 5;
  ASSERT(15, x);
  x -= 3;
  ASSERT(12, x);
  x *= 2;
  ASSERT(24, x);
  x /= 5;
  ASSERT(4, x);
  x %= 3;
  ASSERT(1, x);
  x <<= 4;
  ASSERT(16, x);
  x >>= 2;
  ASSERT(4, x);
  x |= 3;
  ASSERT(7, x);
  x &= 5;
  ASSERT(5, x);
  x ^= 1;
  ASSERT(4, x);

  // Increment and decrement.
  int i = 5;
  ASSERT(5, i++);
  ASSERT(6, i);
  ASSERT(7, ++i);
  ASSERT(7, i--);
  ASSERT(5, --i);
  long l = 1;
  l += 0x100000000;
  ASSERT(0x100000001, l);

  // Integer promotions and conversions.
  char c = 'A';
  ASSERT(66, c + 1);
  ASSERT(4, sizeof(c + 1));
  ASSERT(1, sizeof(c));
  short s = -2;
  ASSERT(-2, s);
  ASSERT(65534, (unsigned short)s);
  ASSERT(-1, (signed char)255);
  ASSERT(255, (unsigned char)-1);
  ASSERT(0x12345678, (int)0x1234567812345678);
  ASSERT(0x5678, (unsigned short)0x12345678);
  ASSERT(-1, (long)(int)-1);
  ASSERT(4294967295, (long)(unsigned)-1);
  ASSERT(1, (_Bool)256);
  ASSERT(1, (bool)0.5);
  ASSERT(0, (bool)0);
  ASSERT(8, sizeof(1L + 1));
  ASSERT(8, sizeof(1u + 1L));

  // 64-bit arithmetic.
  long big = 1L << 62;
  ASSERT(INT64_MIN, (long)((unsigned long)big * 2));
  ASSERT(INT64_MAX, INT64_MAX);
  ASSERT(INT64_MIN, -INT64_MAX - 1);
  unsigned long ul = ULONG_MAX;
  ASSERT(ULONG_MAX / 3, ul / 3);
  ASSERT(0, ul % 3);
  long neg = -9;
  ASSERT(-4, neg / 2);
  ASSERT(-1, neg % 2);

  // Function calls in expressions.
  ASSERT(10, add(3, 7));
  ASSERT(21, add(add(1, 2), add(add(3, 4), add(5, 6))));

  // Character constants.
  ASSERT(97, 'a');
  ASSERT(10, '\n');
  ASSERT(0, '\0');
  ASSERT(-1, '\xff');
  ASSERT(0x41, '\101');

  return test_done();
}
