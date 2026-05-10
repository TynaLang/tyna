#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  void *data;
  int64_t len;
  int64_t cap; // total bytes allocated
  int64_t elem_size; // size of each element in bytes
} TynaArray;

void __tyna_array_new(TynaArray *out, int64_t elem_size) {
  if (!out)
    return;

  if (elem_size <= 0)
    elem_size = 1;

  out->len = 0;
  out->elem_size = elem_size;
  out->cap = 4 * elem_size;
  out->data = malloc((size_t)(out->cap));
}

void __tyna_array_with_capacity(TynaArray *out, int64_t cap,
                                int64_t elem_size) {
  if (!out)
    return;

  if (cap < 0)
    cap = 0;
  if (elem_size <= 0)
    elem_size = 1;

  out->len = 0;
  out->elem_size = elem_size;
  out->cap = cap * elem_size;
  out->data = cap > 0 ? malloc((size_t)(out->cap)) : NULL;
}

void __tyna_array_push(TynaArray *arr, void *element, int64_t elem_size) {
  (void)elem_size; // unused - elem_size is stored in arr->elem_size
  if (!arr || !element)
    return;

  int64_t es = arr->elem_size;
  if (es <= 0)
    es = 1;

  if (arr->cap <= 0) {
    arr->cap = 4 * es;
    arr->data = malloc((size_t)(arr->cap));
    if (!arr->data)
      return;
  }

  int64_t used_bytes = arr->len * es;
  if (used_bytes + es > arr->cap) {
    int64_t new_cap = arr->cap * 2;
    void *resized = realloc(arr->data, (size_t)new_cap);
    if (!resized)
      return;
    arr->data = resized;
    arr->cap = new_cap;
  }

  int8_t *dest = (int8_t *)arr->data + used_bytes;
  memcpy(dest, element, (size_t)es);
  arr->len++;
}

void __tyna_array_from_stack(TynaArray *out, void *stack_ptr, int64_t count,
                             int64_t elem_size) {
  if (!out || !stack_ptr || count < 0 || elem_size <= 0)
    return;

  out->len = count;
  out->cap = count;
  if (count == 0) {
    out->data = NULL;
    return;
  }

  out->data = malloc((size_t)(count * elem_size));
  if (!out->data)
    return;
  memcpy(out->data, stack_ptr, (size_t)(count * elem_size));
}

void __tyna_array_free(TynaArray *arr) {
  if (!arr)
    return;

  if (arr->data) {
    free(arr->data);
    arr->data = NULL;
  }
  arr->len = 0;
  arr->cap = 0;
}