// Pointers, arrays and function pointers.
#include <stddef.h>

#include "test.h"

static int square(int x) { return x * x; }
static int twice(int x) { return x * 2; }
static int apply(int (*f)(int), int v) { return f(v); }

static void swap(int *a, int *b) {
  int t = *a;
  *a = *b;
  *b = t;
}

static int sum_array(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}

static int matrix_trace(int m[3][3]) {
  return m[0][0] + m[1][1] + m[2][2];
}

typedef int (*BinOp)(int, int);
static int op_add(int a, int b) { return a + b; }
static int op_mul(int a, int b) { return a * b; }

int main(void) {
  int x = 3;
  int *p = &x;
  *p = 10;
  ASSERT(10, x);
  int **pp = &p;
  **pp = 20;
  ASSERT(20, x);

  int a = 1, b = 2;
  swap(&a, &b);
  ASSERT(2, a);
  ASSERT(1, b);

  // Pointer arithmetic.
  int arr[5] = {10, 20, 30, 40, 50};
  int *q = arr;
  ASSERT(30, *(q + 2));
  ASSERT(30, q[2]);
  ASSERT(30, 2 [q]);
  q += 3;
  ASSERT(40, *q);
  ASSERT(30, q[-1]);
  ASSERT(3, q - arr);
  ASSERT(-3, arr - q);
  q++;
  ASSERT(50, *q);
  q--;
  q--;
  ASSERT(30, *q);
  ASSERT(1, q > arr);
  ASSERT(1, q >= arr + 2);
  ASSERT(0, q < arr);
  ASSERT(1, &arr[4] == arr + 4);
  ASSERT(150, sum_array(arr, 5));

  // Arrays and sizeof.
  ASSERT(20, sizeof arr);
  ASSERT(5, sizeof arr / sizeof arr[0]);
  ASSERT(8, sizeof &arr);
  ASSERT(8, sizeof(int *));
  int (*parr)[5] = &arr;
  ASSERT(40, (*parr)[3]);
  ASSERT(20, sizeof *parr);

  // Multidimensional arrays.
  int m[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
  ASSERT(15, matrix_trace(m));
  ASSERT(6, m[1][2]);
  ASSERT(12, sizeof m[0]);
  int *flat = &m[0][0];
  ASSERT(8, flat[7]);
  int grid[2][3][4];
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 4; k++)
        grid[i][j][k] = i * 100 + j * 10 + k;
  ASSERT(123, grid[1][2][3]);
  ASSERT(96, sizeof grid);

  // Pointers to struct members and char pointers.
  char buf[8] = "abcdefg";
  char *c = buf;
  ASSERT('a', *c++);
  ASSERT('b', *c);
  ASSERT('c', *++c);
  ASSERT('g', *(buf + 6));
  long *lp = (long *)buf;
  ASSERT(8, sizeof *lp);
  void *vp = buf;
  ASSERT('a', *(char *)vp);
  ASSERT(1, (char *)vp + 1 == buf + 1);

  // Function pointers.
  int (*fp)(int) = square;
  ASSERT(49, fp(7));
  ASSERT(49, (*fp)(7));
  fp = &twice;
  ASSERT(14, fp(7));
  ASSERT(81, apply(square, 9));
  BinOp ops[] = {op_add, op_mul};
  ASSERT(7, ops[0](3, 4));
  ASSERT(12, ops[1](3, 4));
  BinOp *opp = ops;
  ASSERT(12, (*(opp + 1))(3, 4));

  // Null pointers.
  int *np = nullptr;
  ASSERT(1, np == NULL);
  ASSERT(1, np == 0);
  ASSERT(0, np != nullptr);
  ASSERT(1, !np);

  // Pointer difference type.
  ptrdiff_t diff = &arr[4] - &arr[1];
  ASSERT(3, diff);
  ASSERT(8, sizeof diff);

  // Pointer to array element through casts.
  unsigned char bytes[4] = {0x78, 0x56, 0x34, 0x12};
  ASSERT(0x12345678, *(unsigned int *)bytes);

  // Assigning through dereferenced post-increment.
  int dst[3];
  int *w = dst;
  *w++ = 1;
  *w++ = 2;
  *w++ = 3;
  ASSERT(6, dst[0] + dst[1] + dst[2]);
  ASSERT(3, w - dst);

  return test_done();
}
