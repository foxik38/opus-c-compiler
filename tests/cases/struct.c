// Structs, unions, bit-fields and passing aggregates by value (SysV ABI).
#include <stddef.h>
#include <stdint.h>

#include "test.h"

#pragma pack(push, 1)
struct Packed1 { int first; long wide; char last; };
#pragma pack(pop)
_Pragma("pack(push, 4)")
struct Packed4 { char c; double d; };
_Pragma("pack(pop)")
struct Natural { char c; double d; };
struct __attribute__((packed)) AttrPacked { char c; int i; };

struct Point {
  int x, y;
};

struct Rect {
  struct Point min, max;
};

typedef struct {
  char c;
  int i;
  char d;
} Padded;

typedef struct {
  double x, y;
} Vec2;

typedef struct {
  float a, b, c;
} Float3;

typedef struct {
  long a;
  double b;
} Mixed;

typedef struct {
  long a, b, c, d;
} Big;

typedef struct {
  char name[13];
} Name;

struct Node {
  int value;
  struct Node *next;
};

union Bits {
  uint32_t u;
  float f;
  unsigned char bytes[4];
};

struct Flags {
  unsigned a : 1;
  unsigned b : 3;
  int c : 4;
  unsigned d : 24;
  unsigned long e : 40;
};

struct WithAnon {
  int tag;
  union {
    int i;
    double d;
  };
  struct {
    int x, y;
  };
};

struct Flex {
  int len;
  int data[];
};

struct [[gnu::packed]] Packed {
  char c;
  int i;
};

static struct Point make_point(int x, int y) { return (struct Point){x, y}; }
static int point_sum(struct Point p) { return p.x + p.y; }
static Vec2 vec_add(Vec2 a, Vec2 b) { return (Vec2){a.x + b.x, a.y + b.y}; }
static Float3 f3_scale(Float3 v, float s) { return (Float3){v.a * s, v.b * s, v.c * s}; }
static Mixed mixed_make(long a, double b) { return (Mixed){a, b}; }
static double mixed_sum(Mixed m) { return (double)m.a + m.b; }
static Big big_make(long base) { return (Big){base, base + 1, base + 2, base + 3}; }
static long big_sum(Big b) { return b.a + b.b + b.c + b.d; }
static Name name_of(const char *s) {
  Name n = {};
  strncpy(n.name, s, sizeof n.name - 1);
  return n;
}
static int name_len(Name n) { return (int)strlen(n.name); }

// Struct arguments that no longer fit in registers go on the stack.
static long many_structs(struct Point a, struct Point b, struct Point c, struct Point d, struct Point e,
                         struct Point f, struct Point g) {
  return a.x + b.y + c.x + d.y + e.x + f.y + g.x * 100 + g.y;
}

int main(void) {
  struct Point p = {1, 2};
  ASSERT(1, p.x);
  ASSERT(2, p.y);
  p.x = 10;
  ASSERT(12, p.x + p.y);
  struct Point *pp = &p;
  pp->y = 20;
  ASSERT(20, p.y);
  ASSERT(8, sizeof(struct Point));

  struct Rect r = {{0, 0}, {3, 4}};
  ASSERT(4, r.max.y);
  ASSERT(16, sizeof r);
  struct Rect r2 = r;
  r2.max.x = 30;
  ASSERT(3, r.max.x);
  ASSERT(30, r2.max.x);

  // Layout and padding.
  ASSERT(12, sizeof(Padded));
  ASSERT(4, offsetof(Padded, i));
  ASSERT(8, offsetof(Padded, d));
  ASSERT(4, _Alignof(Padded));
  ASSERT(5, sizeof(struct Packed));
  ASSERT(1, offsetof(struct Packed, i));
  struct Packed pk = {'x', 0x01020304};
  ASSERT(0x01020304, pk.i);

  // Linked structures.
  struct Node n3 = {3, nullptr}, n2 = {2, &n3}, n1 = {1, &n2};
  int total = 0;
  for (struct Node *n = &n1; n; n = n->next)
    total += n->value;
  ASSERT(6, total);

  // Unions.
  union Bits bits;
  bits.f = 1.0f;
  ASSERT(0x3f800000, bits.u);
  bits.u = 0x11223344;
  ASSERT(0x44, bits.bytes[0]);
  ASSERT(4, sizeof(union Bits));

  // Bit-fields.
  struct Flags fl = {};
  fl.a = 1;
  fl.b = 5;
  fl.c = -3;
  fl.d = 0xabcdef;
  fl.e = 0xffffffffffUL;
  ASSERT(1, fl.a);
  ASSERT(5, fl.b);
  ASSERT(-3, fl.c);
  ASSERT(0xabcdef, fl.d);
  ASSERT(0xffffffffffL, fl.e);
  fl.b = 9; // truncated to 3 bits
  ASSERT(1, fl.b);
  fl.c = 7;
  fl.c++;
  ASSERT(-8, fl.c); // wraps within 4 signed bits (implementation-defined, as GCC)
  ASSERT(1, fl.a);
  ASSERT(0xabcdef, fl.d);
  fl.b += 2;
  ASSERT(3, fl.b);
  ASSERT(3, (fl.b = 11));

  // Anonymous members.
  struct WithAnon wa = {.tag = 1, .i = 42, .x = 5, .y = 6};
  ASSERT(42, wa.i);
  ASSERT(11, wa.x + wa.y);
  wa.d = 2.5;
  ASSERT_DBL(2.5, wa.d);

  // Flexible array member.
  struct Flex *f = malloc(sizeof(struct Flex) + 4 * sizeof(int));
  f->len = 4;
  for (int i = 0; i < f->len; i++)
    f->data[i] = i * i;
  ASSERT(9, f->data[3]);
  ASSERT(4, sizeof(struct Flex));
  free(f);

  // Passing and returning by value.
  struct Point mp = make_point(7, 8);
  ASSERT(15, point_sum(mp));
  ASSERT(3, point_sum(make_point(1, 2)));
  Vec2 v = vec_add((Vec2){1.5, 2.5}, (Vec2){0.25, 0.75});
  ASSERT_DBL(1.75, v.x);
  ASSERT_DBL(3.25, v.y);
  Float3 f3 = f3_scale((Float3){1, 2, 3}, 2.0f);
  ASSERT_DBL(6.0, f3.c);
  ASSERT_DBL(2.0, f3.a);
  Mixed mx = mixed_make(40, 2.5);
  ASSERT_DBL(42.5, mixed_sum(mx));
  Big bg = big_make(10);
  ASSERT(46, big_sum(bg));
  ASSERT(13, big_make(13).a);
  ASSERT(5, name_len(name_of("occ12")));
  ASSERT(12, name_len(name_of("abcdefghijklmnop")));
  ASSERT(1 + 2 + 3 + 4 + 5 + 6 + 700 + 8,
         many_structs(make_point(1, 0), make_point(0, 2), make_point(3, 0), make_point(0, 4), make_point(5, 0),
                      make_point(0, 6), make_point(7, 8)));

  // Struct assignment through pointers and arrays of structs.
  struct Point pts[3] = {{1, 1}, {2, 2}, {3, 3}};
  pts[0] = pts[2];
  ASSERT(3, pts[0].x);
  struct Point *pt = &pts[1];
  *pt = (struct Point){9, 9};
  ASSERT(9, pts[1].y);
  ASSERT(24, sizeof pts);

  // Compound literal lvalues.
  int *ip = (int[]){10, 20, 30};
  ASSERT(20, ip[1]);
  ASSERT(2, ((struct Point){1, 2}).y);

  // Nested member access through struct returned by function.
  ASSERT(8, make_point(7, 8).y);

  // An unsigned bit-field narrower than int promotes to int, so arithmetic
  // on it stays signed (found by Csmith).
  struct {
    unsigned small : 14;
    unsigned full : 32;
  } pf = {3, 3};
  short negative = -16890;
  ASSERT(1, (pf.small | 0x10) > negative);
  ASSERT(0, (pf.full | 0x10) > negative);    // unsigned int: -16890 converts to a huge value
  ASSERT(1, _Generic(pf.small + 0, int: 1, default: 0));
  ASSERT(1, _Generic(pf.full + 0, unsigned: 1, default: 0));
  ASSERT(-4, -pf.small - 1);

  // #pragma pack and __attribute__((packed)) change the layout, even after
  // glibc's <sys/cdefs.h> has tried to define __attribute__ away (found by
  // Csmith).
  ASSERT(13, sizeof(struct Packed1));
  ASSERT(4, offsetof(struct Packed1, wide));
  ASSERT(1, alignof(struct Packed1));
  ASSERT(12, sizeof(struct Packed4));
  ASSERT(4, offsetof(struct Packed4, d));
  ASSERT(16, sizeof(struct Natural));
  ASSERT(5, sizeof(struct AttrPacked));
  struct Packed1 packed = {1, 2, 3};
  packed.wide += 40;
  ASSERT(42, packed.wide);
  ASSERT(3, packed.last);

  return test_done();
}
