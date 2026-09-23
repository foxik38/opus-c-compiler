// Errors: use of an undeclared identifier.
int main(void) {
  return missing + 1; // expect-error: use of undeclared identifier 'missing'
}
