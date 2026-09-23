// part-of: multifile.c
extern int main_value(void);

int shared_counter;
const char shared_name[] = "library";

struct Config {
  int width, height;
} lib_config = {640, 480};

static int multiply(int a, int b) { return a * b; }
int (*lib_op)(int, int) = multiply;

int lib_add(int a, int b) { return a + b; }

int lib_bump(void) { return ++shared_counter; }

int lib_uses_main(void) { return main_value(); }
