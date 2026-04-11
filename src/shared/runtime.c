#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t rank;
  void *data;
  int64_t *dims;
} FatPtr;

// Returns 0 for equal, -1 or 1 for unequal
int32_t __tyl_compare_arrays(const FatPtr *a, const FatPtr *b,
                             size_t elem_size) {
  // For strings, rank is always 1, but let's be safe.
  // We compare the total number of elements.
  int64_t a_len = (a->rank > 0 && a->dims) ? a->dims[0] : 0;
  int64_t b_len = (b->rank > 0 && b->dims) ? b->dims[0] : 0;

  if (a_len != b_len) {
    return (a_len < b_len) ? -1 : 1;
  }

  if (a->data == b->data) {
    return 0;
  }

  if (a->data == NULL || b->data == NULL) {
    return (a->data == b->data) ? 0 : 1;
  }

  return (int32_t)memcmp(a->data, b->data, a_len * elem_size);
}

void __tyl_str_to_array(FatPtr *out, const FatPtr *str) {
  int64_t str_len = (str->rank > 0 && str->dims) ? str->dims[0] : 0;
  if (str_len <= 0) {
    *out = (FatPtr){0, NULL, NULL};
    return;
  }
  // Allocate str_len + 1 to ensure space for a null terminator if needed,
  // although arrays don't strictly need it, it's safer for string conversions.
  void *new_data = malloc(str_len + 1);
  if (!new_data) {
    *out = (FatPtr){0, NULL, NULL};
    return;
  }
  memcpy(new_data, str->data, str_len);
  ((char *)new_data)[str_len] = '\0';

  // Allocate dims/strides for the new array
  int64_t *new_dims = malloc(sizeof(int64_t) * 2);
  new_dims[0] = str_len; // dim0
  new_dims[1] = 1;       // stride0

  *out = (FatPtr){1, new_data, new_dims};
}

void __tyl_array_init_fixed(void *arr, int64_t elem_size, int64_t fixed_len) {
  if (!arr || fixed_len <= 0 || elem_size <= 0)
    return;

  // Array layout: data, len, cap, rank, dims
  void **data_ptr = (void **)arr;
  int64_t *len_ptr = (int64_t *)((char *)arr + 8);
  int64_t *cap_ptr = (int64_t *)((char *)arr + 16);
  int64_t *rank_ptr = (int64_t *)((char *)arr + 24);
  int64_t **dims_ptr = (int64_t **)((char *)arr + 32);

  if (*data_ptr)
    return;

  *data_ptr = malloc(elem_size * fixed_len);
  *len_ptr = 0;
  *cap_ptr = fixed_len;
  *rank_ptr = 1;
  *dims_ptr = malloc(sizeof(int64_t) * 1);
  if (*dims_ptr)
    (*dims_ptr)[0] = fixed_len;
}

void __tyl_array_to_str(FatPtr *out, const FatPtr *arr) {
  int64_t arr_len = (arr->rank > 0 && arr->dims) ? arr->dims[0] : 0;
  if (arr_len <= 0) {
    // Return empty string
    int64_t *empty_dims = malloc(sizeof(int64_t) * 2);
    empty_dims[0] = 0;
    empty_dims[1] = 1;
    *out = (FatPtr){1, "", empty_dims};
    return;
  }

  // We MUST create a null-terminated copy for the string,
  // because C's printf (used in the backend) expects it.
  char *str_data = malloc(arr_len + 1);
  if (!str_data) {
    *out = (FatPtr){0, NULL, NULL};
    return;
  }
  memcpy(str_data, arr->data, arr_len);
  str_data[arr_len] = '\0';

  int64_t *str_dims = malloc(sizeof(int64_t) * 2);
  str_dims[0] = arr_len;
  str_dims[1] = 1;

  *out = (FatPtr){1, str_data, str_dims};
}
