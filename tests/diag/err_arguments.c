// Errors: wrong number of arguments.
int add(int a, int b) { return a + b; }
int main(void) {
  return add(1); // expect-error: too few arguments to function call, expected 2, have 1
}
