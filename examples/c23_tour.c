// c23_tour.c - a quick tour of the C23 features occ supports.
//
//   occ examples/c23_tour.c --run
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Enumerations with a fixed underlying type.
enum color : uint8_t { RED, GREEN, BLUE };

// constexpr objects are real constant expressions.
constexpr int table_size = 0b1'0000; // binary literal + digit separator

static int squares[table_size];

// Attributes, and unnamed parameters in definitions.
[[nodiscard]] static int twice(int x, int) { return 2 * x; }

[[deprecated("use twice()")]] static int old_twice(int x) { return x + x; }

// The data of a file, embedded at compile time.
static const unsigned char self[] = {
#embed "hello.c"
};

int main(void) {
  for (int i = 0; i < table_size; i++)
    squares[i] = i * i;

  auto answer = twice(21, 0); // type inferred as int
  typeof(answer) copy = answer;
  printf("answer: %d (copy %d), squares[15] = %d\n", answer, copy, squares[15]);

  bool ok = true; // bool, true and false are keywords
  int *p = nullptr;
  printf("ok = %d, p is %s\n", ok, p == nullptr ? "nullptr" : "set");

  // Overflow-checked arithmetic.
  int32_t sum;
  if (ckd_add(&sum, INT32_MAX, 1))
    puts("INT32_MAX + 1 overflows (detected by ckd_add)");

  enum color c = BLUE;
  static_assert(sizeof(enum color) == 1); // message is optional in C23
  printf("BLUE = %d, sizeof(enum color) = %zu\n", c, sizeof c);

  printf("embedded hello.c is %zu bytes and starts with \"%.9s\"\n", sizeof self, (const char *)self);
  printf("old_twice(4) = %d\n", old_twice(4)); // occ warns: -Wdeprecated
  return 0;
}
