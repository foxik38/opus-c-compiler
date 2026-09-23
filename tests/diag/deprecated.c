// Deprecated and obsolescent features.

[[deprecated]] int old_function(void);
[[deprecated("use new_function() instead")]] int older_function(void);
_Noreturn void stop(void); // expect-warning: '_Noreturn' is obsolescent in C23; use [[noreturn]]

#define LIMIT 10
#define LIMIT 20 // expect-warning: 'LIMIT' macro redefined

#warning "this build is experimental" // expect-warning: #warning "this build is experimental"

int main(void) {
  int a = old_function();   // expect-warning: 'old_function' is deprecated
  int b = older_function(); // expect-warning: 'older_function' is deprecated: use new_function() instead
  long double ld = 1.0;     // expect-warning: 'long double' is implemented as 'double' by occ
  int mc = 'ab';            // expect-warning: multi-character character constant
  return a + b + (int)ld + mc + LIMIT;
}
