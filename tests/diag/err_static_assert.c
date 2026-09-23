// Errors: a failing static assertion.
static_assert(sizeof(int) == 8, "int must be 64-bit"); // expect-error: static assertion failed: int must be 64-bit
int main(void) { return 0; }
