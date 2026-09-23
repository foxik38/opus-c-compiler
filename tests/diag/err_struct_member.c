// Errors: accessing a member that does not exist.
struct Point { int x, y; };
int main(void) {
  struct Point p = {1, 2};
  return p.z; // expect-error: no member named 'z' in 'struct Point'
}
