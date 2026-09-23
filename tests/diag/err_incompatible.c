// Errors: incompatible types in assignment.
struct A { int x; };
int main(void) {
  struct A a = {1};
  int n = a; // expect-error: incompatible types when initializing 'int' from 'struct A'
  return n;
}
