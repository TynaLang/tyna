#include "tyna/utils.h"

void *xmalloc(size_t size) {
  void *ptr = malloc(size);
  if (!ptr) {
    panic("Failed to allocate memory");
  }
  return ptr;
}

void *xrealloc(void *ptr, size_t new_size) {
  void *new_ptr = realloc(ptr, new_size);
  if (!new_ptr) {
    panic("Failed to reallocate memory");
  }
  return new_ptr;
}

char *xstrdup(const char *s) {
  char *dup = strdup(s);
  if (!dup) {
    panic("Failed to duplicate string");
  }
  return dup;
}

void *xcalloc(size_t count, size_t size) {
  void *ptr = calloc(count, size);
  if (!ptr) {
    panic("Failed to allocate memory");
  }
  return ptr;
}