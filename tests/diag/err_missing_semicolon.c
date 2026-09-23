// Errors: syntax.
int main(void) {
  int x = 1
  return x; // expect-error: expected ','
}
