#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
  char *ptr;
  int64_t len;
} String;

extern const char *__tyna_as_c_ptr(String s);
extern int64_t __tyna_str_len(String s);
extern void *__tyna_string_alloc_buffer(int64_t size);
extern int32_t __tyna_string_arena_active(void);

#define TYNA_FS_WRITE_TRUNC 0
#define TYNA_FS_WRITE_APPEND 1
#define TYNA_FS_WRITE_CREATE_NEW 2

static int tyna_open_write_mode(int32_t mode) {
  int flags = O_WRONLY | O_CLOEXEC;
  if (mode == TYNA_FS_WRITE_APPEND) {
    return flags | O_CREAT | O_APPEND;
  }
  if (mode == TYNA_FS_WRITE_CREATE_NEW) {
    return flags | O_CREAT | O_EXCL;
  }
  return flags | O_CREAT | O_TRUNC;
}

int32_t __tyna_fs_exists(String path) {
  const char *c_path = __tyna_as_c_ptr(path);
  struct stat st;
  return stat(c_path, &st) == 0 ? 1 : 0;
}

void __tyna_fs_metadata(String path, int64_t *size_out, int32_t *is_file_out,
                        int32_t *is_dir_out, int64_t *modified_out,
                        int32_t *err_out) {
  if (size_out)
    *size_out = -1;
  if (is_file_out)
    *is_file_out = 0;
  if (is_dir_out)
    *is_dir_out = 0;
  if (modified_out)
    *modified_out = 0;
  if (err_out)
    *err_out = 0;

  const char *c_path = __tyna_as_c_ptr(path);
  struct stat st;
  if (stat(c_path, &st) != 0) {
    if (err_out)
      *err_out = errno;
    return;
  }

  if (size_out)
    *size_out = (int64_t)st.st_size;
  if (is_file_out)
    *is_file_out = S_ISREG(st.st_mode) ? 1 : 0;
  if (is_dir_out)
    *is_dir_out = S_ISDIR(st.st_mode) ? 1 : 0;
  if (modified_out)
    *modified_out = (int64_t)st.st_mtime;
}

String __tyna_fs_read_all(String path, int32_t *err_out) {
  if (err_out)
    *err_out = 0;

  const char *c_path = __tyna_as_c_ptr(path);
  int fd = open(c_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    if (err_out)
      *err_out = errno;
    return (String){NULL, 0};
  }

  int64_t cap = 8192;
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
    cap = st.st_size;
  }

  int64_t len = 0;
  char *buf = malloc((size_t)cap + 1);
  if (!buf) {
    if (err_out)
      *err_out = ENOMEM;
    close(fd);
    return (String){NULL, 0};
  }

  while (1) {
    if (len == cap) {
      int64_t new_cap = cap * 2;
      char *new_buf = realloc(buf, (size_t)new_cap + 1);
      if (!new_buf) {
        free(buf);
        close(fd);
        if (err_out)
          *err_out = ENOMEM;
        return (String){NULL, 0};
      }
      buf = new_buf;
      cap = new_cap;
    }

    ssize_t n = read(fd, buf + len, (size_t)(cap - len));
    if (n == 0) {
      break;
    }
    if (n < 0) {
      free(buf);
      close(fd);
      if (err_out)
        *err_out = errno;
      return (String){NULL, 0};
    }
    len += (int64_t)n;
  }

  close(fd);
  buf[len] = '\0';
  return (String){buf, len};
}

int64_t __tyna_fs_read(String path, char *buf, int64_t cap, int32_t *err_out) {
  if (err_out)
    *err_out = 0;

  if (!buf || cap <= 0) {
    if (err_out)
      *err_out = EINVAL;
    return -1;
  }

  const char *c_path = __tyna_as_c_ptr(path);
  int fd = open(c_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    if (err_out)
      *err_out = errno;
    return -1;
  }

  ssize_t n = read(fd, buf, (size_t)cap);
  if (n < 0) {
    if (err_out)
      *err_out = errno;
    close(fd);
    return -1;
  }

  if (close(fd) != 0) {
    if (err_out)
      *err_out = errno;
    return -1;
  }

  return (int64_t)n;
}

static int tyna_fs_build_temp_path(const char *path, char **out_path) {
  const char *dir_path = ".";
  int64_t dir_len = 1;
  const char *slash = strrchr(path, '/');
  if (slash) {
    if (slash == path) {
      dir_path = "/";
      dir_len = 1;
    } else {
      dir_path = path;
      dir_len = (int64_t)(slash - path);
    }
  }

  int need_slash = (dir_path[dir_len - 1] != '/');
  const char *suffix = ".tyna.tmp.XXXXXX";
  size_t template_len = (size_t)dir_len + (need_slash ? 1 : 0) + strlen(suffix);
  char *tmp_path = malloc(template_len + 1);
  if (!tmp_path)
    return -1;

  memcpy(tmp_path, dir_path, (size_t)dir_len);
  size_t offset = (size_t)dir_len;
  if (need_slash) {
    tmp_path[offset++] = '/';
  }
  memcpy(tmp_path + offset, suffix, strlen(suffix));
  tmp_path[template_len] = '\0';
  *out_path = tmp_path;
  return 0;
}

int32_t __tyna_fs_write_atomic(String path, String data, int32_t *err_out) {
  if (err_out)
    *err_out = 0;

  const char *c_path = __tyna_as_c_ptr(path);
  if (!c_path || *c_path == '\0') {
    if (err_out)
      *err_out = EINVAL;
    return -1;
  }

  char *tmp_path = NULL;
  if (tyna_fs_build_temp_path(c_path, &tmp_path) != 0) {
    if (err_out)
      *err_out = ENOMEM;
    return -1;
  }

  int fd = mkstemp(tmp_path);
  if (fd == -1) {
    if (err_out)
      *err_out = errno;
    free(tmp_path);
    return -1;
  }

  if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
    int saved = errno;
    close(fd);
    if (err_out)
      *err_out = saved;
    free(tmp_path);
    return -1;
  }

  const char *bytes = __tyna_as_c_ptr(data);
  int64_t len = __tyna_str_len(data);
  int64_t written = 0;
  int32_t result = 0;

  while (written < len) {
    ssize_t n = write(fd, bytes + written, (size_t)(len - written));
    if (n <= 0) {
      result = errno;
      break;
    }
    written += (int64_t)n;
  }

  if (result == 0 && fsync(fd) != 0) {
    result = errno;
  }

  if (close(fd) != 0 && result == 0) {
    result = errno;
  }

  if (result == 0) {
    if (rename(tmp_path, c_path) != 0) {
      result = errno;
    }
  }

  if (result == 0) {
    const char *dir_path = ".";
    const char *slash = strrchr(c_path, '/');
    if (slash) {
      if (slash == c_path) {
        dir_path = "/";
      } else {
        size_t len = (size_t)(slash - c_path);
        if (len < 4096) {
          char dir_copy[4096];
          memcpy(dir_copy, c_path, len);
          dir_copy[len] = '\0';
          dir_path = dir_copy;
          int dirfd = open(dir_copy, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
          if (dirfd != -1) {
            fsync(dirfd);
            close(dirfd);
          }
        }
      }
    } else {
      int dirfd = open(dir_path, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
      if (dirfd != -1) {
        fsync(dirfd);
        close(dirfd);
      }
    }
  }

  if (result != 0) {
    int saved = errno;
    unlink(tmp_path);
    if (err_out)
      *err_out = result;
    free(tmp_path);
    errno = saved;
    return -1;
  }

  free(tmp_path);
  return 0;
}

int32_t __tyna_fs_write_all(String path, String data, int32_t mode,
                            int32_t *err_out) {
  if (err_out)
    *err_out = 0;

  const char *c_path = __tyna_as_c_ptr(path);
  int fd = open(c_path, tyna_open_write_mode(mode), 0644);
  if (fd == -1) {
    if (err_out)
      *err_out = errno;
    return -1;
  }

  const char *bytes = __tyna_as_c_ptr(data);
  int64_t len = __tyna_str_len(data);
  int64_t written = 0;

  while (written < len) {
    ssize_t n = write(fd, bytes + written, (size_t)(len - written));
    if (n <= 0) {
      if (err_out)
        *err_out = errno;
      close(fd);
      return -1;
    }
    written += (int64_t)n;
  }

  if (close(fd) != 0) {
    if (err_out)
      *err_out = errno;
    return -1;
  }

  return 0;
}

int32_t __tyna_fs_remove(String path) {
  const char *c_path = __tyna_as_c_ptr(path);
  if (unlink(c_path) == 0)
    return 0;
  return errno;
}

int32_t __tyna_fs_rename(String from, String to) {
  const char *c_from = __tyna_as_c_ptr(from);
  const char *c_to = __tyna_as_c_ptr(to);
  if (rename(c_from, c_to) == 0)
    return 0;
  return errno;
}

int32_t __tyna_fs_create_dir_all(String path) {
  const char *c_path = __tyna_as_c_ptr(path);
  size_t path_len = strlen(c_path);
  if (path_len == 0)
    return EINVAL;
  if (path_len >= 4096)
    return ENAMETOOLONG;

  char tmp[4096];
  memcpy(tmp, c_path, path_len + 1);

  for (size_t i = 1; i < path_len; i++) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (tmp[0] != '\0' && mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return errno;
      }
      tmp[i] = '/';
    }
  }

  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    return errno;
  }

  return 0;
}
