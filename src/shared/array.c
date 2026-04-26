#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  void *data;
  int64_t len;
  int64_t cap;
} TynaArray;

void __tyna_array_new(TynaArray *out, int64_t elem_size) {
  if (!out)
    return;

  if (elem_size <= 0)
    elem_size = 1;

  out->len = 0;
  out->cap = 4;
  out->data = malloc((size_t)(out->cap * elem_size));
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
  out->cap = cap;
  out->data = cap > 0 ? malloc((size_t)(cap * elem_size)) : NULL;
}

void __tyna_array_push(TynaArray *arr, void *element, int64_t elem_size) {
  if (!arr || !element || elem_size <= 0)
    return;

  if (arr->cap <= 0) {
    arr->cap = 4;
    arr->data = malloc((size_t)(arr->cap * elem_size));
    if (!arr->data)
      return;
  }

  if (arr->len >= arr->cap) {
    arr->cap *= 2;
    void *resized = realloc(arr->data, (size_t)(arr->cap * elem_size));
    if (!resized)
      return;
    arr->data = resized;
  }

  int8_t *dest = (int8_t *)arr->data + (arr->len * elem_size);
  memcpy(dest, element, (size_t)elem_size);
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