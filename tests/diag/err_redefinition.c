// Errors: redefinition in the same scope.
int main(void) {
  int value = 1;
  int value = 2; // expect-error: redefinition of 'value'
  return value;
}
