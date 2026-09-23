// A warning-free program: occ must not produce false positives here.
// expect-clean
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int argc_like(void) { return 3; }

typedef struct {
  const char *name;
  int score;
} Entry;

static int by_score(const void *a, const void *b) {
  const Entry *x = a, *y = b;
  return (y->score > x->score) - (y->score < x->score);
}

static unsigned hash(const char *s) {
  unsigned h = 2166136261u;
  while (*s)
    h = (h ^ (unsigned char)*s++) * 16777619u;
  return h;
}

// Patterns from real code (Lua) that must not warn: an assignment whose
// value is used is a read, and promoted unsigned narrow types are never
// negative, so comparing them with unsigned values is safe.
static int count_open(const int *levels, int level) {
  int n = 0, current;
  while ((current = *levels++) >= level)
    n++;
  return n;
}

static int fits(unsigned char byte, size_t size, unsigned short half, unsigned long wide) {
  return (byte <= size) + (half != wide) + ((argc_like() & 0x7f) < size) + (size > (half < 3));
}

int main(int argc, char **argv) {
  Entry entries[] = {{"ada", 90}, {"bob", 72}, {"cy", 85}};
  size_t n = sizeof entries / sizeof entries[0];
  qsort(entries, n, sizeof entries[0], by_score);
  for (size_t i = 0; i < n; i++)
    printf("%zu. %-5s %3d %08x\n", i + 1, entries[i].name, entries[i].score, hash(entries[i].name));

  char *copy = malloc(strlen(argv[0]) + 1);
  if (!copy)
    return 1;
  strcpy(copy, argv[0]);
  int total = 0;
  for (int i = 0; i < argc; i++)
    total += (int)strlen(argv[i]);
  unsigned mask = 0xffu;
  if (mask > 0u && total >= 0)
    total &= (int)mask;
  free(copy);
  while (total > 1000)
    total /= 2;
  static const int levels[] = {5, 4, 1};
  total += count_open(levels, 2) + fits(7, n, 9, 10);
  return total > 255 ? 1 : 0;
}
