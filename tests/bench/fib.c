// Recursive Fibonacci: call overhead and integer arithmetic.
#include <stdio.h>

static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void) {
  printf("fib(35) = %ld\n", fib(35));
  return 0;
}
