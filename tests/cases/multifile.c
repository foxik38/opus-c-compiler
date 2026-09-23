// Several translation units linked together: extern data and functions.
// extra: multifile_lib.c
#include "test.h"

extern int shared_counter;
extern const char shared_name[];
extern int lib_add(int a, int b);
extern int lib_bump(void);
extern int (*lib_op)(int, int);
extern struct Config {
  int width, height;
} lib_config;

int from_main = 5;
int main_value(void) { return from_main * 2; }

int main(void) {
  ASSERT(0, shared_counter);
  ASSERT(1, lib_bump());
  ASSERT(2, lib_bump());
  ASSERT(2, shared_counter);
  shared_counter = 10;
  ASSERT(11, lib_bump());
  ASSERT_STR("library", shared_name);
  ASSERT(7, lib_add(3, 4));
  ASSERT(12, lib_op(3, 4));
  ASSERT(640, lib_config.width);
  ASSERT(480, lib_config.height);
  return test_done();
}
