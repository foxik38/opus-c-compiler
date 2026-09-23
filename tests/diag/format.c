// printf/scanf format string checking.
#include <stdio.h>

int main(void) {
  long l = 1;
  double d = 2.0;
  int i = 3;
  char name[16];
  printf("%d\n", l);           // expect-warning: format specifies type 'int' but the argument has type 'long'
  printf("%s\n", i);           // expect-warning: format specifies type 'char *' but the argument has type 'int'
  printf("%d %d\n", i);        // expect-warning: more '%' conversions than data arguments
  printf("%d\n", i, i);        // expect-warning: data argument not used by format string
  printf("%f\n", i);           // expect-warning: format specifies type 'double' but the argument has type 'int'
  printf("%zu %ld %lld %p %c %5.2f %%\n", sizeof i, l, (long long)l, (void *)&i, 'x', d); // fine
  printf("%*d\n", 4, i);       // fine
  scanf("%d", i);              // expect-warning: format specifies a pointer but the argument has type 'int'
  scanf("%d", &d);             // expect-warning: format specifies type 'int *' but the argument has type 'double *'
  scanf("%lf %15s", &d, name); // fine
  return 0;
}
