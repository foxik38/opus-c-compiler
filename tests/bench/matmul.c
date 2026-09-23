// Dense matrix multiplication: floating point and array indexing.
#include <stdio.h>

enum { N = 320 };
static double a[N][N], b[N][N], c[N][N];

int main(void) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      a[i][j] = (double)(i * j % 7) / 3.0;
      b[i][j] = (double)(i + 2 * j) / 11.0;
    }
  for (int i = 0; i < N; i++)
    for (int k = 0; k < N; k++) {
      double aik = a[i][k];
      for (int j = 0; j < N; j++)
        c[i][j] += aik * b[k][j];
    }
  double trace = 0;
  for (int i = 0; i < N; i++)
    trace += c[i][i];
  printf("trace = %.6f\n", trace);
  return 0;
}
