#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t rank;
  void *data;
  int64_t *dims;
} FatPtr;

typedef struct {
  uint64_t ref_count;
  uint64_t hash;
  char data[];
} StringHeader;

typedef struct {
  char *ptr;
  int64_t len;
} String;

static String string_make(const char *data, int64_t len) {
  StringHeader *hdr = malloc(sizeof(StringHeader) + len + 1);
  if (!hdr) {
    return (String){NULL, 0};
  }
  hdr->ref_count = 1;
  hdr->hash = 0;
  memcpy(hdr->data, data, len);
  hdr->data[len] = '\0';
  return (String){hdr->data, len};
}

static StringHeader *string_header_from_ptr(char *ptr) {
  return (StringHeader *)(ptr - offsetof(StringHeader, data));
}

String __tyl_str_concat(String a, String b) {
  int64_t len = a.len + b.len;
  StringHeader *hdr = malloc(sizeof(StringHeader) + len + 1);
  if (!hdr) {
    return (String){NULL, 0};
  }
  hdr->ref_count = 1;
  hdr->hash = 0;
  if (a.ptr && a.len > 0) {
    memcpy(hdr->data, a.ptr, a.len);
  }
  if (b.ptr && b.len > 0) {
    memcpy(hdr->data + a.len, b.ptr, b.len);
  }
  hdr->data[len] = '\0';
  return (String){hdr->data, len};
}

String __tyl_str_intern(String s) {
  // Basic stub: return the same string without interning.
  // Full intern pool will be added later.
  return s;
}

uint64_t __tyl_str_hash(String s) {
  if (!s.ptr || s.len == 0)
    return 0;
  StringHeader *hdr = string_header_from_ptr(s.ptr);
  if (hdr->hash)
    return hdr->hash;

  uint64_t hash = 1469598103934665603ULL;
  for (int64_t i = 0; i < s.len; i++) {
    hash ^= (uint64_t)(unsigned char)s.ptr[i];
    hash *= 1099511628211ULL;
  }
  if (hash == 0)
    hash = 1;
  hdr->hash = hash;
  return hash;
}

int32_t __tyl_str_equals(String a, String b) {
  if (a.len != b.len)
    return 0;
  if (a.ptr == b.ptr)
    return 1;
  if (!a.ptr || !b.ptr)
    return 0;
  return memcmp(a.ptr, b.ptr, a.len) == 0;
}

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

void __tyl_str_to_array(FatPtr *out, const String *str) {
  if (!out || !str || !str->ptr || str->len <= 0) {
    if (out) {
      *out = (FatPtr){0, NULL, NULL};
    }
    return;
  }

  int64_t str_len = str->len;
  void *new_data = malloc(str_len + 1);
  if (!new_data) {
    *out = (FatPtr){0, NULL, NULL};
    return;
  }
  memcpy(new_data, str->ptr, str_len);
  ((char *)new_data)[str_len] = '\0';

  int64_t *new_dims = malloc(sizeof(int64_t) * 2);
  if (!new_dims) {
    free(new_data);
    *out = (FatPtr){0, NULL, NULL};
    return;
  }
  new_dims[0] = str_len;
  new_dims[1] = 1;

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

int64_t *__tyl_array_clone_dims(int64_t rank, const int64_t *dims) {
  if (rank <= 0 || !dims)
    return NULL;

  int64_t *copy = malloc(sizeof(int64_t) * (size_t)rank);
  if (!copy)
    return NULL;

  memcpy(copy, dims, sizeof(int64_t) * (size_t)rank);
  return copy;
}

void __tyl_array_to_str(String *out, const FatPtr *arr) {
  if (!out || !arr || !arr->data || arr->rank <= 0 || !arr->dims) {
    if (out) {
      *out = (String){NULL, 0};
    }
    return;
  }

  int64_t arr_len = arr->dims[0];
  if (arr_len <= 0) {
    *out = (String){NULL, 0};
    return;
  }

  StringHeader *hdr = malloc(sizeof(StringHeader) + arr_len + 1);
  if (!hdr) {
    *out = (String){NULL, 0};
    return;
  }

  hdr->ref_count = 1;
  hdr->hash = 0;
  memcpy(hdr->data, arr->data, arr_len);
  hdr->data[arr_len] = '\0';

  *out = (String){hdr->data, arr_len};
}
