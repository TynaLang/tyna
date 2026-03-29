#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *data;
  size_t len;
} StringView;

#define SV_FMT "%.*s"
#define SV_ARG(sv) (int)(sv).len, (sv).data

static inline StringView sv_from_parts(const char *data, size_t len) {
  return (StringView){data, len};
}

static inline StringView sv_from_cstr(const char *ptr) {
  return (StringView){ptr, strlen(ptr)};
}

static inline bool sv_eq(StringView a, StringView b) {
  if (a.len != b.len)
    return false;
  return memcmp(a.data, b.data, a.len) == 0;
}

static inline bool sv_eq_cstr(StringView a, const char *b) {
  size_t blen = strlen(b);
  if (a.len != blen)
    return false;
  return memcmp(a.data, b, blen) == 0;
}

// Extract a null-terminated string (caller must free)
static inline char *sv_to_cstr(StringView sv) {
  char *res = malloc(sv.len + 1);
  if (res) {
    memcpy(res, sv.data, sv.len);
    res[sv.len] = '\0';
  }
  return res;
}

typedef struct {
  void **items;
  size_t len;
  size_t capacity;
} List;

void List_init(List *list);
void List_push(List *list, void *item);
void *List_get(List *list, size_t index);
void List_free(List *list, int free_items);

void panic(const char *msg);

char *read_file(const char *filepath);

#endif // UTILS_H
