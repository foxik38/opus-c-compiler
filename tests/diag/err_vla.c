// Errors: variable length arrays (optional in C23) are not supported.
int main(int argc, char **argv) {
  int buffer[argc]; // expect-error: variable length arrays are not supported by occ
  (void)argv;
  return buffer[0];
}
