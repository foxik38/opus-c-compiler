// Errors: K&R-style definitions were removed in C23.
int old(a, b) // expect-error: unknown type name 'a' (K&R-style parameter lists were removed in C23)
  int a, b;
{ return a + b; }
