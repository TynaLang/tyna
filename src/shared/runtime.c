#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t rank;
  void *data;
  int64_t *dims;
} FatPtr;

typedef struct {
  char *ptr;
  int64_t len;
} String;

typedef struct TylStringArena {
  char *base;
  int64_t cap;
  int64_t used;
  struct TylStringArena *parent;
} TylStringArena;

static _Thread_local TylStringArena *tyl_current_arena = NULL;
static int64_t tyl_string_alloc_count = 0;
static int64_t tyl_string_free_count = 0;
static int64_t tyl_string_heap_alloc_count = 0;

#define TYL_SSO_PTR_TAG ((uintptr_t)1ULL << 63)
#define TYL_SSO_MAX_LEN 15

static void *tyl_string_arena_alloc(int64_t size) {
  if (size <= 0)
    return NULL;

  if (!tyl_current_arena) {
    void *ptr = malloc((size_t)size);
    if (ptr)
      tyl_string_heap_alloc_count++;
    return ptr;
  }

  if (!tyl_current_arena->base) {
    int64_t initial = 4096;
    if (initial < size)
      initial = size;
    tyl_current_arena->base = malloc((size_t)initial);
    if (!tyl_current_arena->base)
      return NULL;
    tyl_current_arena->cap = initial;
    tyl_current_arena->used = 0;
  }

  int64_t needed = tyl_current_arena->used + size;
  if (needed > tyl_current_arena->cap) {
    int64_t new_cap = tyl_current_arena->cap;
    while (new_cap < needed)
      new_cap *= 2;
    char *new_base = realloc(tyl_current_arena->base, (size_t)new_cap);
    if (!new_base)
      return NULL;
    tyl_current_arena->base = new_base;
    tyl_current_arena->cap = new_cap;
  }

  void *result = tyl_current_arena->base + tyl_current_arena->used;
  tyl_current_arena->used += size;
  return result;
}

static int tyl_string_arena_owns(const void *ptr) {
  if (!ptr)
    return 0;

  const char *cptr = (const char *)ptr;
  TylStringArena *arena = tyl_current_arena;
  while (arena) {
    if (arena->base) {
      const char *base = arena->base;
      int owns = (cptr >= base && cptr < base + arena->cap);
      if (owns) {
        return 1;
      }
    }
    arena = arena->parent;
  }

  return 0;
}

void __tyl_string_arena_push(void) {
  TylStringArena *arena = malloc(sizeof(TylStringArena));
  if (!arena)
    return;
  arena->base = NULL;
  arena->cap = 0;
  arena->used = 0;
  arena->parent = tyl_current_arena;
  tyl_current_arena = arena;
}

void __tyl_string_arena_pop(void) {
  if (!tyl_current_arena)
    return;

  TylStringArena *arena = tyl_current_arena;
  tyl_current_arena = arena->parent;
  free(arena->base);
  free(arena);
}

static int tyl_str_is_sso(String s) {
  return ((uintptr_t)s.ptr & TYL_SSO_PTR_TAG) != 0;
}

static int64_t tyl_str_sso_len(String s) { return (int64_t)(s.len & 0xFF); }

static void tyl_str_sso_copy(String s, char *out) {
  uint64_t low = (uintptr_t)s.ptr & ~TYL_SSO_PTR_TAG;
  uint64_t high = (uint64_t)s.len >> 8;
  int64_t len = tyl_str_sso_len(s);
  for (int i = 0; i < 7 && i < len; i++) {
    out[i] = (char)((low >> (i * 8)) & 0xFF);
  }
  for (int i = 7; i < len; i++) {
    out[i] = (char)((high >> ((i - 7) * 8)) & 0xFF);
  }
}

static String tyl_str_from_buffer(const char *src, int64_t len) {
  if (len <= TYL_SSO_MAX_LEN) {
    uintptr_t low = 0;
    uint64_t high = 0;
    for (int i = 0; i < 7 && i < len; i++) {
      low |= ((uintptr_t)(unsigned char)src[i]) << (i * 8);
    }
    for (int i = 7; i < len; i++) {
      high |= ((uint64_t)(unsigned char)src[i]) << ((i - 7) * 8);
    }
    String s;
    s.ptr = (char *)(low | TYL_SSO_PTR_TAG);
    s.len = ((uint64_t)len & 0xFF) | (high << 8);
    return s;
  }
  char *data = malloc((size_t)len + 1);
  if (!data)
    return (String){NULL, 0};
  tyl_string_alloc_count++;
  tyl_string_heap_alloc_count++;
  memcpy(data, src, (size_t)len);
  data[len] = '\0';
  return (String){data, len};
}

static int64_t tyl_str_real_len(String s) {
  return tyl_str_is_sso(s) ? tyl_str_sso_len(s) : s.len;
}

static const char *tyl_str_data(String s, char *tmp) {
  if (!s.ptr || tyl_str_real_len(s) == 0)
    return "";
  if (tyl_str_is_sso(s)) {
    int64_t len = tyl_str_sso_len(s);
    tyl_str_sso_copy(s, tmp);
    tmp[len] = '\0';
    return tmp;
  }
  return s.ptr;
}

String __tyl_str_concat(String a, String b) {
  int64_t a_len = tyl_str_real_len(a);
  int64_t b_len = tyl_str_real_len(b);
  int64_t len = a_len + b_len;

  char tmp_a[TYL_SSO_MAX_LEN + 1];
  char tmp_b[TYL_SSO_MAX_LEN + 1];
  const char *a_data = tyl_str_data(a, tmp_a);
  const char *b_data = tyl_str_data(b, tmp_b);

  if (len <= TYL_SSO_MAX_LEN) {
    char tmp_concat[TYL_SSO_MAX_LEN + 1];
    if (a_len > 0) {
      memcpy(tmp_concat, a_data, (size_t)a_len);
    }
    if (b_len > 0) {
      memcpy(tmp_concat + a_len, b_data, (size_t)b_len);
    }
    tmp_concat[len] = '\0';
    return tyl_str_from_buffer(tmp_concat, len);
  }

  char *data = malloc((size_t)len + 1);
  if (!data) {
    return (String){NULL, 0};
  }
  tyl_string_alloc_count++;

  if (a_len > 0) {
    memcpy(data, a_data, (size_t)a_len);
  }
  if (b_len > 0) {
    memcpy(data + a_len, b_data, (size_t)b_len);
  }
  data[len] = '\0';

  return (String){data, len};
}

#define INTERN_POOL_CAPACITY 1024
static struct {
  String pool[INTERN_POOL_CAPACITY];
  int count;
} intern_pool = {0};

static TylStringArena global_intern_arena = {NULL, 0, 0, NULL};

static void *tyl_intern_arena_alloc(int64_t size) {
  if (size <= 0)
    return NULL;

  if (!global_intern_arena.base) {
    int64_t initial = 4096;
    if (initial < size)
      initial = size;
    global_intern_arena.base = malloc((size_t)initial);
    if (!global_intern_arena.base)
      return NULL;
    global_intern_arena.cap = initial;
    global_intern_arena.used = 0;
  }

  int64_t needed = global_intern_arena.used + size;
  if (needed > global_intern_arena.cap) {
    int64_t new_cap = global_intern_arena.cap;
    while (new_cap < needed)
      new_cap *= 2;
    char *new_base = realloc(global_intern_arena.base, (size_t)new_cap);
    if (!new_base)
      return NULL;
    global_intern_arena.base = new_base;
    global_intern_arena.cap = new_cap;
  }

  void *result = global_intern_arena.base + global_intern_arena.used;
  global_intern_arena.used += size;
  return result;
}

String __tyl_str_intern(String s) {
  int64_t s_len = tyl_str_real_len(s);
  if (s_len == 0)
    return (String){NULL, 0};

  char tmp[TYL_SSO_MAX_LEN + 1];
  const char *s_data = tyl_str_data(s, tmp);

  for (int i = 0; i < intern_pool.count; i++) {
    if (intern_pool.pool[i].len == s_len &&
        memcmp(intern_pool.pool[i].ptr, s_data, (size_t)s_len) == 0) {
      return intern_pool.pool[i];
    }
  }

  if (intern_pool.count >= INTERN_POOL_CAPACITY) {
    fprintf(stderr, "tyl: intern pool exhausted\n");
    abort();
  }

  char *copy = tyl_intern_arena_alloc(s_len + 1);
  if (!copy) {
    fprintf(stderr, "tyl: out of memory during intern\n");
    abort();
  }
  memcpy(copy, s_data, (size_t)s_len);
  copy[s_len] = '\0';

  intern_pool.pool[intern_pool.count] = (String){copy, s_len};
  intern_pool.count++;
  return intern_pool.pool[intern_pool.count - 1];
}

uint64_t __tyl_str_hash(String s) {
  int64_t len = tyl_str_real_len(s);
  if (len == 0)
    return 0;

  char tmp[TYL_SSO_MAX_LEN + 1];
  const char *data = tyl_str_data(s, tmp);
  uint64_t hash = 1469598103934665603ULL;
  for (int64_t i = 0; i < len; i++) {
    hash ^= (uint64_t)(unsigned char)data[i];
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

int32_t __tyl_str_equals(String a, String b) {
  int64_t a_len = tyl_str_real_len(a);
  int64_t b_len = tyl_str_real_len(b);
  if (a_len != b_len)
    return 0;
  if (a.ptr == b.ptr && a_len == b_len)
    return 1;
  if (a_len == 0 && b_len == 0)
    return 1;

  char tmp_a[TYL_SSO_MAX_LEN + 1];
  char tmp_b[TYL_SSO_MAX_LEN + 1];
  const char *a_data = tyl_str_data(a, tmp_a);
  const char *b_data = tyl_str_data(b, tmp_b);
  return memcmp(a_data, b_data, (size_t)a_len) == 0;
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
  if (!out || !str) {
    if (out) {
      *out = (FatPtr){0, NULL, NULL};
    }
    return;
  }

  int64_t str_len = tyl_str_real_len(*str);
  if (str_len <= 0) {
    *out = (FatPtr){0, NULL, NULL};
    return;
  }

  char tmp[TYL_SSO_MAX_LEN + 1];
  const char *str_data = tyl_str_data(*str, tmp);
  void *new_data = malloc(str_len + 1);
  if (!new_data) {
    *out = (FatPtr){0, NULL, NULL};
    return;
  }
  memcpy(new_data, str_data, (size_t)str_len);
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

  if (arr_len <= TYL_SSO_MAX_LEN) {
    *out = tyl_str_from_buffer((const char *)arr->data, arr_len);
    return;
  }

  char *data = malloc((size_t)arr_len + 1);
  if (!data) {
    *out = (String){NULL, 0};
    return;
  }

  memcpy(data, arr->data, (size_t)arr_len);
  data[arr_len] = '\0';

  *out = (String){data, arr_len};
}

typedef struct {
  char *data;
  int64_t len;
  int64_t cap;
} TylStringBuf;

static void *__tyl_string_alloc_buffer(int64_t size) {
  void *ptr = tyl_string_arena_alloc(size);
  if (!ptr)
    return NULL;
  return ptr;
}

void __tyl_string_new(TylStringBuf *out) {
  if (!out)
    return;

  const int64_t initial = 16;
  char *d = __tyl_string_alloc_buffer(initial);
  if (!d) {
    *out = (TylStringBuf){NULL, 0, 0};
    return;
  }
  tyl_string_alloc_count++;
  d[0] = '\0';
  *out = (TylStringBuf){d, 0, initial};
}

void __tyl_string_push(TylStringBuf *buf, String piece) {
  if (!buf || !piece.ptr)
    return;

  int64_t piece_len = tyl_str_real_len(piece);
  if (piece_len <= 0)
    return;

  if (!buf->data || buf->cap <= 0) {
    TylStringBuf init_buf;
    __tyl_string_new(&init_buf);
    *buf = init_buf;
    if (!buf->data)
      return;
  }

  int64_t need = buf->len + piece_len;
  if (need > buf->cap) {
    int64_t ncap = buf->cap;
    while (ncap < need)
      ncap *= 2;

    char *nd;
    if (tyl_string_arena_owns(buf->data)) {
      nd = __tyl_string_alloc_buffer(ncap);
      if (!nd)
        return;
      memcpy(nd, buf->data, (size_t)buf->len);
    } else {
      nd = realloc(buf->data, (size_t)ncap);
      if (!nd)
        return;
    }

    buf->data = nd;
    buf->cap = ncap;
  }

  if (tyl_str_is_sso(piece)) {
    char tmp[TYL_SSO_MAX_LEN + 1];
    const char *data = tyl_str_data(piece, tmp);
    memcpy(buf->data + buf->len, data, (size_t)piece_len);
  } else {
    memcpy(buf->data + buf->len, piece.ptr, (size_t)piece_len);
  }

  buf->len += piece_len;
  buf->data[buf->len] = '\0';
}

void __tyl_string_free(TylStringBuf *buf) {
  if (!buf || !buf->data)
    return;

  if (!tyl_string_arena_owns(buf->data)) {
    free(buf->data);
  }
  tyl_string_free_count++;
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

int64_t __tyl_test_string_alloc_count(void) { return tyl_string_alloc_count; }

int64_t __tyl_test_string_free_count(void) { return tyl_string_free_count; }

int64_t __tyl_test_string_heap_alloc_count(void) {
  return tyl_string_heap_alloc_count;
}

void __tyl_test_string_reset_counts(void) {
  tyl_string_alloc_count = 0;
  tyl_string_free_count = 0;
  tyl_string_heap_alloc_count = 0;
}

String __tyl_string_into_str(TylStringBuf *buf) {
  if (!buf || !buf->data)
    return (String){NULL, 0};

  if (buf->len <= 0) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return (String){NULL, 0};
  }

  int64_t len = buf->len;
  if (len <= TYL_SSO_MAX_LEN) {
    char tmp[TYL_SSO_MAX_LEN + 1];
    memcpy(tmp, buf->data, (size_t)len);
    tmp[len] = '\0';
    String result = tyl_str_from_buffer(tmp, len);
    if (!tyl_string_arena_owns(buf->data)) {
      free(buf->data);
      tyl_string_free_count++;
    }
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return result;
  }

  if (tyl_string_arena_owns(buf->data)) {
    char *copy = malloc((size_t)len + 1);
    if (!copy)
      return (String){NULL, 0};
    tyl_string_alloc_count++;
    tyl_string_heap_alloc_count++;
    memcpy(copy, buf->data, (size_t)len);
    copy[len] = '\0';
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return (String){copy, len};
  }

  String result = (String){buf->data, buf->len};
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  return result;
}

String __tyl_string_clone_str(TylStringBuf *buf) {
  if (!buf || !buf->data || buf->len <= 0)
    return (String){NULL, 0};

  if (buf->len <= TYL_SSO_MAX_LEN) {
    char tmp[TYL_SSO_MAX_LEN + 1];
    memcpy(tmp, buf->data, (size_t)buf->len);
    tmp[buf->len] = '\0';
    return tyl_str_from_buffer(tmp, buf->len);
  }

  char *copy = malloc((size_t)buf->len + 1);
  if (!copy)
    return (String){NULL, 0};
  tyl_string_alloc_count++;
  tyl_string_heap_alloc_count++;

  memcpy(copy, buf->data, (size_t)buf->len);
  copy[buf->len] = '\0';
  return (String){copy, buf->len};
}

const char *__tyl_as_c_ptr(String s) {
  if (!s.ptr || tyl_str_real_len(s) == 0)
    return "";

  if (!tyl_str_is_sso(s))
    return s.ptr;

  static _Thread_local char tmp[4][TYL_SSO_MAX_LEN + 1];
  static _Thread_local int tmp_index = 0;
  int current = tmp_index;
  tmp_index = (tmp_index + 1) % 4;

  int64_t len = tyl_str_sso_len(s);
  tyl_str_sso_copy(s, tmp[current]);
  tmp[current][len] = '\0';
  return tmp[current];
}

int32_t __tyl_ptr_eq(const void *a, const void *b) { return a == b ? 1 : 0; }
