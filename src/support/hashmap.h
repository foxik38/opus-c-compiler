// hashmap.h - string-keyed hash map (open addressing, linear probing).
//
// Keys are byte strings given as (pointer, length) so that tokens can be used
// as keys without copying. The map stores its own copy of every key.
#pragma once

#include "support/common.h"

typedef struct {
  char *key;
  int keylen;
  void *val;
} HashEntry;

typedef struct {
  HashEntry *buckets;
  int capacity;
  int used; // live entries + tombstones
} HashMap;

void *hashmap_get(const HashMap *map, const char *key);
void *hashmap_get2(const HashMap *map, const char *key, int keylen);
void hashmap_put(HashMap *map, const char *key, void *val);
void hashmap_put2(HashMap *map, const char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, const char *key);
void hashmap_delete2(HashMap *map, const char *key, int keylen);
