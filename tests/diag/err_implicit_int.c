// Errors: implicit int was removed in C99.
static x = 5; // expect-error: type specifier missing; ISO C99 and later do not support implicit int
int main(void) { return x; }
