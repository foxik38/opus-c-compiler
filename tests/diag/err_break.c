// Errors: break outside of a loop.
int main(void) {
  break; // expect-error: 'break' statement not in loop or switch statement
}
