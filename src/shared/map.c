#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  void *data;
  int64_t len;
  int64_t cap;
} TynaArray;

typedef struct {
  TynaArray buckets;
  int64_t count;
  int64_t capacity;
} TynaMap;

void __tyna_map_init(TynaMap *out, int64_t entry_size) {
  if (!out)
    return;

  if (entry_size <= 0)
    entry_size = 1;

  int64_t initial_capacity = 4;
  out->buckets.len = 0;
  out->buckets.cap = initial_capacity;
  out->buckets.data = calloc((size_t)initial_capacity, (size_t)entry_size);
  out->count = 0;
  out->capacity = initial_capacity;
}
