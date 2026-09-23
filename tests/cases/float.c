// Floating point: arithmetic, conversions, comparisons and libm.
#include <float.h>
#include <math.h>
#include <stdint.h>

#include "test.h"

static double half(double x) { return x / 2; }
static float halff(float x) { return x / 2; }
static double poly(double x) { return 3 * x * x - 2 * x + 1; }

int main(void) {
  double a = 1.5, b = 2.25;
  ASSERT_DBL(3.75, a + b);
  ASSERT_DBL(-0.75, a - b);
  ASSERT_DBL(3.375, a * b);
  ASSERT_DBL(1.5 / 2.25, a / b);
  ASSERT_DBL(-1.5, -a);
  ASSERT_DBL(2.0, half(4));
  ASSERT_DBL(1.25, halff(2.5f));
  ASSERT_DBL(22.0, poly(3));

  float f = 0.1f;
  double d = f;
  ASSERT(1, d != 0.1); // float precision
  ASSERT(1, f == 0.1f);
  ASSERT(4, sizeof(float));
  ASSERT(8, sizeof(double));
  ASSERT(8, sizeof(1.0f + 1.0)); // float + double is double
  ASSERT(4, sizeof(1.0f * 2));

  // Conversions.
  ASSERT(3, (int)3.99);
  ASSERT(-3, (int)-3.99);
  ASSERT(255, (unsigned char)255.9);
  ASSERT_DBL(5.0, (double)5);
  ASSERT_DBL(-7.0, (double)-7L);
  ASSERT_DBL(4294967295.0, (double)4294967295u);
  unsigned long big = 0xffffffffffffffffUL;
  ASSERT_DBL(18446744073709551615.0, (double)big);
  unsigned long b63 = 1UL << 63;
  ASSERT_DBL(9223372036854775808.0, (double)b63);
  double huge = 1e19;
  ASSERT(10000000000000000000UL, (unsigned long)huge);
  ASSERT(9007199254740993L - 1, (long)9007199254740992.0);
  float ff = 16777217.0f; // rounds to 16777216
  ASSERT(16777216, (int)ff);
  ASSERT_DBL(0.5, (float)0.5);
  int i = 7;
  ASSERT_DBL(3.5, i / 2.0);
  ASSERT(3, i / 2);
  long long ll = -1;
  ASSERT_DBL(-1.0, (float)ll);

  // Comparisons, including NaN.
  double nan = NAN;
  ASSERT(0, nan == nan);
  ASSERT(1, nan != nan);
  ASSERT(0, nan < 1.0);
  ASSERT(0, nan >= 1.0);
  ASSERT(1, isnan(nan));
  ASSERT(1, nan ? 1 : 0); // NaN is true
  ASSERT(0, !nan);
  ASSERT(1, 1.0 < 2.0);
  ASSERT(1, 2.0 >= 2.0);
  ASSERT(0, -0.0 < 0.0);
  ASSERT(1, -0.0 == 0.0);
  double inf = INFINITY;
  ASSERT(1, inf > DBL_MAX);
  ASSERT(1, -inf < -DBL_MAX);
  ASSERT(1, isinf(1.0 / 0.0));
  if (0.0)
    ASSERT(0, 1);
  int taken = 0;
  if (0.5)
    taken = 1;
  ASSERT(1, taken);

  // Compound assignment and increments.
  double acc = 0;
  for (int k = 0; k < 10; k++)
    acc += 0.5;
  ASSERT_DBL(5.0, acc);
  acc *= 2;
  ASSERT_DBL(10.0, acc);
  acc++;
  ASSERT_DBL(11.0, acc);
  double old = acc--;
  ASSERT_DBL(11.0, old);
  ASSERT_DBL(10.0, acc);
  float fa = 1.0f;
  fa /= 4;
  ASSERT_DBL(0.25, fa);
  float fi = 0.1f;
  float fold = fi++;
  ASSERT(1, fold == 0.1f);
  ASSERT(1, fi == 1.1f);

  // libm.
  ASSERT_DBL(3.0, sqrt(9.0));
  ASSERT_DBL(1.0, cos(0.0));
  ASSERT_DBL(8.0, pow(2.0, 3.0));
  ASSERT_DBL(2.0, floor(2.7));
  ASSERT_DBL(-2.0, ceil(-2.7));
  ASSERT_DBL(2.5, fabs(-2.5));
  ASSERT_DBL(1.0, fmod(7.0, 3.0));
  ASSERT_DBL(3.14159265358979323846, 4 * atan(1.0));
  ASSERT_DBL(3.0f, sqrtf(9.0f));

  // printf formatting of doubles.
  char buf[64];
  snprintf(buf, sizeof buf, "%.3f %g %e", 3.14159, 0.5, 12345.0);
  ASSERT_STR("3.142 0.5 1.234500e+04", buf);
  snprintf(buf, sizeof buf, "%f", (double)1.5f);
  ASSERT_STR("1.500000", buf);

  // Float constants and limits.
  ASSERT(1, FLT_EPSILON > 0);
  ASSERT(1, DBL_MIN > 0);
  ASSERT_DBL(0x1.8p1, 3.0);
  ASSERT_DBL(1e-3, 0.001);
  ASSERT_DBL(1.5e3, 1500.0);

  return test_done();
}
