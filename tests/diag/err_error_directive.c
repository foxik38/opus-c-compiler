// Errors: #error.
#if !defined(CONFIGURED)
#error "run ./configure first" // expect-error: #error "run ./configure first"
#endif
int main(void) { return 0; }
