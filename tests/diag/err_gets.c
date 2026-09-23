// Errors: gets() was removed in C11.
#include <stdio.h>
int main(void) {
  char buf[16];
  gets(buf); // expect-error: 'gets' was removed in C11 because it cannot be used safely; use fgets()
  return 0;
}
