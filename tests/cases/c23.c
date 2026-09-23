// C23 language features.
// occ-only: uses #embed, <stdckdint.h> and other features GCC 13 lacks.
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>

#include "test.h"

// constexpr objects are usable in constant expressions.
constexpr int SIZE = 4;
constexpr double SCALE = 2.5;
static int table[SIZE * 2];
static_assert(sizeof table == 32);
static_assert(SIZE == 4, "constexpr");

// Enumerations with a fixed underlying type.
enum Small : unsigned char { S_A, S_B = 200, S_C };
enum Wide : long { W_BIG = 0x100000000 };
static_assert(sizeof(enum Small) == 1);
static_assert(sizeof(enum Wide) == 8);

// Attributes.
[[nodiscard]] static int must_use(int x) { return x + 1; }
[[deprecated("use new_api")]] static int old_api(void) { return 1; }
[[noreturn]] static void die(void) { exit(3); }
[[maybe_unused]] static int helper;

// Unnamed parameters in definitions.
static int second(int, int b) { return b; }

// Empty parameter list means no parameters.
static int no_params() { return 7; }

// Digit separators and binary literals.
static_assert(1'000'000 == 1000000);
static_assert(0b1010'1010 == 0xAA);
static_assert(0x7fff'ffff == 2147483647);

// true/false/bool are keywords.
static bool flag = true;

static const unsigned char blob[] = {
#embed "embed_data.txt"
};

static const char blob_limited[] = {
#embed "embed_data.txt" limit(5)
    , 0};

#if __has_embed("embed_data.txt")
static int has_embed = 1;
#else
static int has_embed = 0;
#endif

#ifdef __occ__
#elifdef NEVER_DEFINED
#error "unreachable"
#endif
#ifndef __STDC_VERSION__
#error "missing __STDC_VERSION__"
#elifndef __x86_64__
#error "not x86-64"
#endif

int main(void) {
  // auto type inference.
  auto i = 42;
  auto d = 1.5;
  auto p = &table[1];
  ASSERT(4, sizeof i);
  ASSERT(8, sizeof d);
  ASSERT(1, p - table);
  auto s = "str";
  ASSERT(8, sizeof s); // arrays decay

  // typeof and typeof_unqual.
  const int ci = 5;
  typeof(ci) c2 = 6;
  typeof_unqual(ci) m = 7;
  m++;
  ASSERT(8, m);
  ASSERT(11, ci + c2);
  typeof(int[3]) arr3 = {1, 2, 3};
  ASSERT(12, sizeof arr3);

  // nullptr and nullptr_t.
  int *np = nullptr;
  nullptr_t n = nullptr;
  ASSERT(1, np == n);
  ASSERT(8, sizeof(nullptr_t));
  void *vp = nullptr;
  ASSERT(1, !vp);

  // bool.
  bool b = 5;
  ASSERT(1, b);
  ASSERT(1, flag);
  ASSERT(1, sizeof(bool));
  ASSERT(1, true + false);
  b = false;
  ASSERT(0, b);

  // constexpr locals.
  constexpr int local_size = 3;
  int local_arr[local_size] = {};
  ASSERT(12, sizeof local_arr);
  ASSERT_DBL(5.0, SCALE * 2);

  // Enums.
  enum Small sm = S_C;
  ASSERT(201, sm);
  ASSERT(0x100000000, W_BIG);

  // Attributes on declarations and statements.
  ASSERT(2, must_use(1));
  ASSERT(9, second(1, 9));
  ASSERT(7, no_params());
  int x = 1;
  switch (x) {
  case 1:
    x = 10;
    [[fallthrough]];
  case 2:
    x++;
  }
  ASSERT(11, x);

  // Empty initializer.
  struct {
    int a;
    double b;
  } zero = {};
  ASSERT(0, zero.a);
  int zi = {};
  ASSERT(0, zi);

  // Checked arithmetic (<stdckdint.h>).
  int r;
  ASSERT(0, ckd_add(&r, 2, 3));
  ASSERT(5, r);
  ASSERT(1, ckd_add(&r, INT32_MAX, 1));
  ASSERT(1, ckd_mul(&r, 1 << 20, 1 << 20));
  ASSERT(0, ckd_sub(&r, -5, 10));
  ASSERT(-15, r);
  unsigned u;
  ASSERT(1, ckd_sub(&u, 0u, 1u));
  ASSERT(0, ckd_mul(&u, 65536u, 65535u));
  unsigned char uc;
  ASSERT(1, ckd_add(&uc, (unsigned char)200, (unsigned char)100));
  ASSERT(44, uc);
  long lr;
  ASSERT(0, ckd_mul(&lr, 1L << 31, 1L << 31));
  ASSERT(1L << 62, lr);

  // #embed.
  ASSERT(15, sizeof blob);
  ASSERT('o', blob[0]);
  ASSERT('\n', blob[14]);
  ASSERT_STR("occ e", blob_limited);
  ASSERT(1, has_embed);

  // Labels at the end of compound statements and before declarations.
  {
    goto end;
  end:
  }
  {
  before_decl:
    int after = 3;
    ASSERT(3, after);
  }

  // unreachable() from <stddef.h> after a noreturn call is never reached.
  if (x == 0) {
    die();
    unreachable();
  }

  // static_assert inside functions, without a message.
  static_assert(sizeof(int) == 4);

  // __has_c_attribute.
#if __has_c_attribute(nodiscard) >= 202003L && !__has_c_attribute(no_such_attribute)
  int attr_ok = 1;
#else
  int attr_ok = 0;
#endif
  ASSERT(1, attr_ok);
  ASSERT(202311, __STDC_VERSION__);

  // u8 character constants have type unsigned char.
  ASSERT(1, _Generic(u8'a', unsigned char: 1, default: 0));
  (void)old_api;

  return test_done();
}
