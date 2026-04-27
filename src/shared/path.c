#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *ptr;
  int64_t len;
} String;

extern const char *__tyna_as_c_ptr(String s);
extern int64_t __tyna_str_len(String s);

static String tyna_string_copy(const char *src, int64_t len) {
  if (!src || len <= 0) {
    return (String){NULL, 0};
  }

  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    return (String){NULL, 0};
  }

  memcpy(buf, src, (size_t)len);
  buf[len] = '\0';
  return (String){buf, len};
}

String __tyna_path_join(String base, String segment) {
  const char *a = __tyna_as_c_ptr(base);
  const char *b = __tyna_as_c_ptr(segment);
  int64_t a_len = __tyna_str_len(base);
  int64_t b_len = __tyna_str_len(segment);

  if (b_len > 0 && b[0] == '/')
    return tyna_string_copy(b, b_len);
  if (a_len == 0)
    return tyna_string_copy(b, b_len);
  if (b_len == 0)
    return tyna_string_copy(a, a_len);

  int need_slash = a[a_len - 1] != '/';
  int64_t out_len = a_len + b_len + (need_slash ? 1 : 0);
  char *buf = malloc((size_t)out_len + 1);
  if (!buf)
    return (String){NULL, 0};

  memcpy(buf, a, (size_t)a_len);
  int64_t offset = a_len;
  if (need_slash) {
    buf[offset++] = '/';
  }
  memcpy(buf + offset, b, (size_t)b_len);
  buf[out_len] = '\0';
  return (String){buf, out_len};
}

String __tyna_path_dir(String path) {
  const char *p = __tyna_as_c_ptr(path);
  int64_t len = __tyna_str_len(path);
  if (len <= 0)
    return tyna_string_copy(".", 1);

  const char *slash = strrchr(p, '/');
  if (!slash)
    return tyna_string_copy(".", 1);

  if (slash == p)
    return tyna_string_copy("/", 1);

  int64_t out_len = (int64_t)(slash - p);
  return tyna_string_copy(p, out_len);
}

String __tyna_path_filename(String path) {
  const char *p = __tyna_as_c_ptr(path);
  int64_t len = __tyna_str_len(path);
  if (len <= 0)
    return (String){NULL, 0};

  const char *slash = strrchr(p, '/');
  const char *start = slash ? slash + 1 : p;
  int64_t out_len = (int64_t)strlen(start);
  return tyna_string_copy(start, out_len);
}

String __tyna_path_extension(String path) {
  const char *p = __tyna_as_c_ptr(path);
  int64_t len = __tyna_str_len(path);
  if (len <= 0)
    return (String){NULL, 0};

  const char *slash = strrchr(p, '/');
  const char *filename = slash ? slash + 1 : p;
  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename)
    return (String){NULL, 0};

  int64_t out_len = (int64_t)strlen(dot);
  return tyna_string_copy(dot, out_len);
}

String __tyna_path_with_extension(String path, String new_ext) {
  const char *p = __tyna_as_c_ptr(path);
  const char *ext = __tyna_as_c_ptr(new_ext);
  int64_t p_len = __tyna_str_len(path);
  int64_t ext_len = __tyna_str_len(new_ext);

  if (p_len <= 0)
    return (String){NULL, 0};

  const char *slash = strrchr(p, '/');
  const char *filename = slash ? slash + 1 : p;
  const char *dot = strrchr(filename, '.');

  int64_t base_len = p_len;
  if (dot && dot != filename) {
    base_len = (int64_t)(dot - p);
  }

  int need_dot = (ext_len > 0 && ext[0] != '.') ? 1 : 0;
  int64_t out_len = base_len + (ext_len > 0 ? ext_len + need_dot : 0);
  char *buf = malloc((size_t)out_len + 1);
  if (!buf)
    return (String){NULL, 0};

  memcpy(buf, p, (size_t)base_len);
  int64_t offset = base_len;
  if (ext_len > 0) {
    if (need_dot) {
      buf[offset++] = '.';
    }
    memcpy(buf + offset, ext, (size_t)ext_len);
  }

  buf[out_len] = '\0';
  return (String){buf, out_len};
}

int32_t __tyna_path_is_absolute(String path) {
  const char *p = __tyna_as_c_ptr(path);
  int64_t len = __tyna_str_len(path);
  return (len > 0 && p[0] == '/') ? 1 : 0;
}
