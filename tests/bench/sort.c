// Quicksort of pseudo-random integers: pointers, swaps, recursion.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void quicksort(int *a, long lo, long hi) {
  while (lo < hi) {
    int pivot = a[lo + (hi - lo) / 2];
    long i = lo, j = hi;
    while (i <= j) {
      while (a[i] < pivot)
        i++;
      while (a[j] > pivot)
        j--;
      if (i <= j) {
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
        i++;
        j--;
      }
    }
    if (j - lo < hi - i) {
      quicksort(a, lo, j);
      lo = i;
    } else {
      quicksort(a, i, hi);
      hi = j;
    }
  }
}

int main(void) {
  enum { N = 4'000'000 };
  int *a = malloc(N * sizeof *a);
  uint32_t state = 12345;
  for (long i = 0; i < N; i++) {
    state = state * 1103515245u + 12345u; // LCG
    a[i] = (int)(state >> 1);
  }
  quicksort(a, 0, N - 1);
  uint64_t check = 0;
  for (long i = 1; i < N; i++) {
    if (a[i - 1] > a[i]) {
      puts("not sorted!");
      return 1;
    }
    check = check * 31 + (uint64_t)a[i];
  }
  printf("checksum = %llu\n", (unsigned long long)check);
  free(a);
  return 0;
}
