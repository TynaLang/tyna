#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

typedef struct {
  void **items;
  size_t len;
  size_t capacity;
} List;

void List_init(List *list);
void List_push(List *list, void *item);
void List_insert(List *list, size_t index, void *item);
void *List_pop(List *list);
void *List_get(List *list, size_t index);
void List_free(List *list, int free_items);
void *List_remove_at(List *list, size_t index);
int List_index_of(List *list, void *item);

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t new_size);
char *xstrdup(const char *s);

noreturn void panic(const char *msg, ...);

char *read_file(const char *filepath);

typedef struct {
  const char *data;
  size_t len;
} StringView;

#define SV_FMT "%.*s"
#define SV_ARG(sv) (int)(sv).len, (sv).data
#define TEMP_STR_MAX 256

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

static inline const char *sv_to_cstr_temp(StringView sv) {
  static char buf[TEMP_STR_MAX];

  if (sv.len == 0) {
    buf[0] = '\0';
    return buf;
  }

  if (sv.data == NULL) {
    buf[0] = '\0';
    return buf;
  }

  if (sv.len >= TEMP_STR_MAX) {
    panic("StringView too long for temporary buffer");
  }

  size_t copy_len = sv.len < TEMP_STR_MAX - 1 ? sv.len : TEMP_STR_MAX - 1;
  memcpy(buf, sv.data, copy_len);
  buf[copy_len] = '\0';
  return buf;
}

static inline bool sv_contains(StringView sv, char c) {
  for (size_t i = 0; i < sv.len; i++) {
    if (sv.data[i] == c)
      return true;
  }
  return false;
}

static inline bool sv_ends_with(StringView sv, const char *suffix) {
  size_t suffix_len = strlen(suffix);
  if (sv.len < suffix_len)
    return false;
  return memcmp(sv.data + sv.len - suffix_len, suffix, suffix_len) == 0;
}

#endif // UTILS_H
