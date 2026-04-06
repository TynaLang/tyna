#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Matches the backend structure of the fat pointer
typedef struct {
  int64_t len;
  void *data;
} FatPtr;

// Returns 0 for equal, -1 or 1 for unequal
int32_t __tyl_compare_arrays(FatPtr a, FatPtr b, size_t elem_size) {
  if (a.len != b.len) {
    return (a.len < b.len) ? -1 : 1;
  }

  if (a.data == b.data) {
    return 0;
  }

  if (a.data == NULL || b.data == NULL) {
    return (a.data == b.data) ? 0 : 1;
  }

  return (int32_t)memcmp(a.data, b.data, a.len * elem_size);
}

#include <stdlib.h>

FatPtr __tyl_str_to_array(FatPtr str) {
  if (str.len <= 0) {
    return (FatPtr){0, NULL};
  }
  void *new_data = malloc(str.len);
  if (!new_data)
    return (FatPtr){0, NULL};
  memcpy(new_data, str.data, str.len);
  return (FatPtr){str.len, new_data};
}

FatPtr __tyl_array_to_str(FatPtr arr) {
  if (arr.len <= 0) {
    return (FatPtr){0, ""};
  }
  // For now we don't deduplicate at runtime, just return the data as is.
  // In a real system, we might want to intern it or at least ensure it's
  // immutable.
  return arr;
}
