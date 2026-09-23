// Errors: macro invoked with the wrong number of arguments.
#define PAIR(a, b) ((a) + (b))
int main(void) {
  return PAIR(1); // expect-error: macro 'PAIR' requires 2 arguments, but only 1 given
}
