// Open-addressing hash table of strings: memory, strings, hashing.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char key[16];
  int value;
  bool used;
} Slot;

enum { CAP = 1 << 20, KEYS = 600'000 };
static Slot table[CAP];

static uint32_t hash(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}

static Slot *lookup(const char *key) {
  for (uint32_t i = hash(key) & (CAP - 1);; i = (i + 1) & (CAP - 1))
    if (!table[i].used || strcmp(table[i].key, key) == 0)
      return &table[i];
}

int main(void) {
  char key[16];
  for (int round = 0; round < 3; round++)
    for (int i = 0; i < KEYS; i++) {
      snprintf(key, sizeof key, "k%d", i * 7 % KEYS);
      Slot *s = lookup(key);
      if (!s->used) {
        s->used = true;
        strcpy(s->key, key);
      }
      s->value += i & 15;
    }
  long sum = 0;
  int used = 0;
  for (int i = 0; i < CAP; i++)
    if (table[i].used) {
      used++;
      sum += table[i].value;
    }
  printf("keys = %d, sum = %ld\n", used, sum);
  return 0;
}
