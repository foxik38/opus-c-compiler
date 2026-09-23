// hashmap.c - string-keyed hash map with linear probing and tombstones.
#include "support/hashmap.h"

enum { INIT_CAPACITY = 16, LOAD_PERCENT = 70 };

// A deleted slot keeps probing chains intact.
static char TOMBSTONE_KEY;
#define TOMBSTONE (&TOMBSTONE_KEY)

static uint64_t fnv1a(const char *s, int len) {
  uint64_t h = 0xcbf29ce484222325;
  for (int i = 0; i < len; i++) {
    h ^= (unsigned char)s[i];
    h *= 0x100000001b3;
  }
  return h;
}

static bool key_matches(const HashEntry *e, const char *key, int keylen) {
  return e->key && e->key != TOMBSTONE && e->keylen == keylen && memcmp(e->key, key, (size_t)keylen) == 0;
}

static void rehash(HashMap *map) {
  int live = 0;
  for (int i = 0; i < map->capacity; i++)
    if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
      live++;

  int cap = map->capacity;
  while (live * 100 / cap >= 50)
    cap *= 2;

  HashMap fresh = {.buckets = xcalloc((size_t)cap, sizeof(HashEntry)), .capacity = cap};
  for (int i = 0; i < map->capacity; i++) {
    HashEntry *e = &map->buckets[i];
    if (e->key && e->key != TOMBSTONE) {
      // Keys are already owned; move them without copying.
      uint64_t h = fnv1a(e->key, e->keylen);
      for (int j = 0;; j++) {
        HashEntry *slot = &fresh.buckets[(h + (uint64_t)j) % (uint64_t)cap];
        if (!slot->key) {
          *slot = *e;
          break;
        }
      }
      fresh.used++;
    }
  }
  free(map->buckets);
  *map = fresh;
}

static HashEntry *find_entry(const HashMap *map, const char *key, int keylen) {
  if (!map->buckets)
    return nullptr;
  uint64_t h = fnv1a(key, keylen);
  for (int i = 0; i < map->capacity; i++) {
    HashEntry *e = &map->buckets[(h + (uint64_t)i) % (uint64_t)map->capacity];
    if (key_matches(e, key, keylen))
      return e;
    if (!e->key)
      return nullptr;
  }
  return nullptr;
}

void *hashmap_get(const HashMap *map, const char *key) { return hashmap_get2(map, key, (int)strlen(key)); }

void *hashmap_get2(const HashMap *map, const char *key, int keylen) {
  HashEntry *e = find_entry(map, key, keylen);
  return e ? e->val : nullptr;
}

void hashmap_put(HashMap *map, const char *key, void *val) { hashmap_put2(map, key, (int)strlen(key), val); }

void hashmap_put2(HashMap *map, const char *key, int keylen, void *val) {
  if (!map->buckets) {
    map->buckets = xcalloc(INIT_CAPACITY, sizeof(HashEntry));
    map->capacity = INIT_CAPACITY;
  } else if ((map->used + 1) * 100 / map->capacity >= LOAD_PERCENT) {
    rehash(map);
  }

  HashEntry *existing = find_entry(map, key, keylen);
  if (existing) {
    existing->val = val;
    return;
  }

  uint64_t h = fnv1a(key, keylen);
  for (int i = 0;; i++) {
    HashEntry *e = &map->buckets[(h + (uint64_t)i) % (uint64_t)map->capacity];
    if (!e->key || e->key == TOMBSTONE) {
      if (!e->key)
        map->used++;
      e->key = xstrndup(key, (size_t)keylen);
      e->keylen = keylen;
      e->val = val;
      return;
    }
  }
}

void hashmap_delete(HashMap *map, const char *key) { hashmap_delete2(map, key, (int)strlen(key)); }

void hashmap_delete2(HashMap *map, const char *key, int keylen) {
  HashEntry *e = find_entry(map, key, keylen);
  if (e) {
    free(e->key);
    e->key = TOMBSTONE;
    e->val = nullptr;
  }
}
