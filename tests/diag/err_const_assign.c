// Errors: assigning to a const object.
int main(void) {
  const int limit = 10;
  limit = 20; // expect-error: cannot assign to variable 'limit' with const-qualified type 'const int'
  return limit;
}
