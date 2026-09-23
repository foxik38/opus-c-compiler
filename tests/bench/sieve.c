// Sieve of Eratosthenes: loops, byte arrays and branches.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  enum { N = 20'000'000 };
  char *composite = malloc(N + 1);
  long total = 0;
  for (int round = 0; round < 5; round++) {
    memset(composite, 0, N + 1);
    int count = 0;
    for (int i = 2; i <= N; i++) {
      if (composite[i])
        continue;
      count++;
      for (long j = (long)i * i; j <= N; j += i)
        composite[j] = 1;
    }
    total += count;
  }
  printf("primes below %d: %ld\n", N, total / 5);
  free(composite);
  return 0;
}
