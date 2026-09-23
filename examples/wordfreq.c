// wordfreq.c - count word frequencies from stdin with a hash table.
//
//   occ -o prod examples/wordfreq.c && ./wordfreq < README.md
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Entry Entry;
struct Entry {
  Entry *next;
  size_t count;
  char word[];
};

enum { BUCKETS = 4096, TOP = 10 };

static Entry *table[BUCKETS];
static size_t distinct;

static uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}

static void add(const char *word) {
  Entry **slot = &table[fnv1a(word) % BUCKETS];
  for (Entry *e = *slot; e; e = e->next)
    if (strcmp(e->word, word) == 0) {
      e->count++;
      return;
    }
  size_t len = strlen(word);
  Entry *e = malloc(sizeof *e + len + 1);
  if (!e) {
    perror("malloc");
    exit(1);
  }
  memcpy(e->word, word, len + 1);
  e->count = 1;
  e->next = *slot;
  *slot = e;
  distinct++;
}

static int by_count(const void *a, const void *b) {
  const Entry *x = *(Entry *const *)a, *y = *(Entry *const *)b;
  if (x->count != y->count)
    return x->count < y->count ? 1 : -1;
  return strcmp(x->word, y->word);
}

int main(void) {
  char word[64];
  size_t len = 0, total = 0;
  for (int c; (c = getchar()) != EOF;) {
    if (isalpha(c) && len < sizeof word - 1) {
      word[len++] = (char)tolower(c);
    } else if (len) {
      word[len] = '\0';
      add(word);
      total++;
      len = 0;
    }
  }

  Entry **all = malloc(distinct * sizeof *all);
  size_t n = 0;
  for (size_t b = 0; b < BUCKETS; b++)
    for (Entry *e = table[b]; e; e = e->next)
      all[n++] = e;
  qsort(all, n, sizeof *all, by_count);

  printf("%zu words, %zu distinct\n", total, distinct);
  for (size_t i = 0; i < n && i < TOP; i++)
    printf("%6zu  %s\n", all[i]->count, all[i]->word);
  return 0;
}
