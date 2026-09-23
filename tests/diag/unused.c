// Unused declarations and discarded values.

[[nodiscard]] int compute(void);
[[nodiscard("check the status")]] int status(void);

static void helper(void) {} // expect-warning: unused function 'helper'
static int counter;         // expect-warning: unused variable 'counter'

int main(void) {
  int unused;            // expect-warning: unused variable 'unused'
  int only_written;      // expect-warning: variable 'only_written' set but not used
  only_written = 5;
  int cast_away;         // expect-warning: variable 'cast_away' set but not used
  (void)(cast_away = 1);
  int chain_outer, chain_inner; // expect-warning: variable 'chain_outer' set but not used
  chain_outer = chain_inner = 2; // the inner assignment's value is used
  [[maybe_unused]] int fine;
  int x = 1;
  x + 1;                 // expect-warning: expression result unused
  x == 2;                // expect-warning: expression result unused
  compute();             // expect-warning: ignoring return value of function declared with 'nodiscard' attribute
  status();              // expect-warning: ignoring return value of function declared with 'nodiscard' attribute: check the status
  (void)compute();       // explicitly discarded: no warning
  return x;
}
