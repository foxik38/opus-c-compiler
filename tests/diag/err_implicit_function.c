// Errors: implicit function declarations were removed in C99.
int main(void) {
  return helper(3); // expect-error: call to undeclared function 'helper'; ISO C99 and later do not support implicit function declarations
}
