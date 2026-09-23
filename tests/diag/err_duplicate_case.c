// Errors: duplicate case labels.
int main(int argc, char **argv) {
  (void)argv;
  switch (argc) {
  case 1: return 1;
  case 1: return 2; // expect-error: duplicate case value '1'
  }
  return 0;
}
