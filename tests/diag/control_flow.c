// Control-flow warnings.
#include <stdlib.h>

int missing_return(int x) {
  if (x > 0)
    return 1;
} // expect-warning: non-void function 'missing_return' does not return a value in all control paths

int all_paths(int x) { // no warning: every path returns
  if (x)
    return 1;
  else
    return 2;
}

int ends_in_abort(int x) { // no warning: abort() does not return
  if (x)
    return 1;
  abort();
}

int infinite(void) { // no warning: the loop never exits
  for (;;) {
  }
}

int by_switch(int x) { // no warning: default covers all values
  switch (x) {
  case 1:
    return 10;
  default:
    return 20;
  }
}

int main(void) {
  int x = 0;
  if (x = 1)           // expect-warning: using the result of an assignment as a condition without parentheses
    x++;
  if ((x = 2))         // extra parentheses silence it
    x++;
  if (x);              // expect-warning: 'if' statement has an empty body; did you mean to put a statement here?
  while (x > 100);     // expect-warning: 'while' loop has an empty body
  while (x--)
    ;                  // on its own line: intentional
  return missing_return(x) + all_paths(x) + ends_in_abort(x) + infinite() + by_switch(x);
}
