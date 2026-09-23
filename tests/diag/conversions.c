// Suspicious implicit conversions and comparisons.
#include <string.h>

static int sum(int values[10]) {
  return (int)sizeof(values); // expect-warning: 'sizeof' on array function parameter 'values' will return size of 'int *'
}

int main(void) {
  const char *msg = "hi";
  char *mutable_msg = msg;     // expect-warning: initializing 'char *' from 'const char *' discards qualifiers
  int *ip = 42;                // expect-warning: initializing 'int *' from 'int' makes pointer from integer without a cast
  long addr = mutable_msg;     // expect-warning: initializing 'long' from 'char *' makes integer from pointer without a cast
  double d = 1.0;
  int *wrong = &d;             // expect-warning: incompatible pointer types initializing 'int *' from 'double *'
  unsigned count = 3;
  int i = -1;
  if (i < count)               // expect-warning: comparison of integers of different signs: 'int' and 'unsigned int'
    i = 0;
  for (int k = 0; k < strlen(msg); k++) // expect-warning: comparison of integers of different signs: 'int' and 'unsigned long'
    i++;
  if (msg == "hi")             // expect-warning: comparison against a string literal compares addresses; use strcmp()
    i++;
  if (ip == 0) // null pointer constant: fine
    i++;
  return i + (int)addr + *wrong + sum(nullptr);
}
