// Errors: missing header.
#include "no_such_header.h" // expect-error: 'no_such_header.h' file not found
int main(void) { return 0; }
