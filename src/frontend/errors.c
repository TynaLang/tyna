#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tyl/errors.h"
#include "tyl/lexer.h"

static char *path_trim_relative(const char *path, const char *entry_path) {
  if (!path)
    return xstrdup("<unknown>");

  if (!entry_path)
    return xstrdup(path);

  char resolved_path[PATH_MAX];
  char resolved_entry[PATH_MAX];
  if (!realpath(path, resolved_path) || !realpath(entry_path, resolved_entry)) {
    return xstrdup(path);
  }

  char *entry_dir = xstrdup(resolved_entry);
  char *slash = strrchr(entry_dir, '/');
  if (slash) {
    *slash = '\0';
  } else {
    entry_dir[0] = '\0';
  }

  size_t prefix_len = strlen(entry_dir);
  char *result = NULL;
  if (prefix_len > 0 && strncmp(resolved_path, entry_dir, prefix_len) == 0 &&
      resolved_path[prefix_len] == '/') {
    result = xstrdup(resolved_path + prefix_len + 1);
  } else {
    result = xstrdup(path);
  }

  free(entry_dir);
  return result;
}

void ErrorHandler_init(ErrorHandler *eh, const char *src, const char *path,
                       const char *entry_path, const char *path_prefix) {
  List_init(&eh->errors);
  eh->src = src;
  eh->path = path_trim_relative(path, entry_path);
  eh->path_prefix = path_prefix ? xstrdup(path_prefix) : NULL;
  eh->has_errors = 0;
}

void ErrorHandler_free(ErrorHandler *eh) {
  free(eh->path_prefix);
  for (size_t i = 0; i < eh->errors.len; i++) {
    free(eh->errors.items[i]);
  }
  free(eh->path);
}

void ErrorHandler_show_all(ErrorHandler *eh) {
  if (!eh->has_errors)
    return;
  for (size_t i = 0; i < eh->errors.len; i++) {
    fprintf(stderr, "%s\n", (char *)eh->errors.items[i]);
  }
}

void ErrorHandler_report(ErrorHandler *eh, Location loc, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);

  ErrorHandler_report_level(eh, loc, LEVEL_ERROR, "%s", msg_buf);
}

void ErrorHandler_report_level(ErrorHandler *eh, Location loc, ErrorLevel level,
                               const char *fmt, ...) {
  if (level == LEVEL_ERROR) {
    eh->has_errors = 1;
  }

  char msg_buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);

  // Extract the line from src
  const char *line_start = eh->src;
  size_t current_line = 1;
  while (current_line < loc.line && *line_start != '\0') {
    if (*line_start == '\n') {
      current_line++;
    }
    line_start++;
  }

  const char *line_end = line_start;
  while (*line_end != '\n' && *line_end != '\0') {
    line_end++;
  }

  size_t line_len = line_end - line_start;
  char *source_line = xmalloc(line_len + 1);
  if (source_line) {
    strncpy(source_line, line_start, line_len);
    source_line[line_len] = '\0';
  } else {
    source_line = xstrdup("");
  }

  // Create the col pointer `   ^`
  size_t caret_pad = (loc.col > 1) ? loc.col - 1 : 0;
  if (caret_pad > line_len)
    caret_pad = line_len; // Cap at line length

  char *caret_str = xmalloc(caret_pad + 2);
  if (caret_str) {
    memset(caret_str, ' ', caret_pad);
    caret_str[caret_pad] = '^';
    caret_str[caret_pad + 1] = '\0';
  } else {
    caret_str = xstrdup("^");
  }

  const char *level_str = "Error";
  const char *color_prefix = "\x1b[31;1m"; // Red for Error

  if (level == LEVEL_WARNING) {
    level_str = "Warning";
    color_prefix = "\x1b[33;1m"; // Yellow for Warning
  } else if (level == LEVEL_INFO) {
    level_str = "Info";
    color_prefix = "\x1b[34;1m"; // Blue for Info
  }

  const char *display_path = eh->path ? eh->path : "<unknown>";
  char *prefixed_path = NULL;
  if (eh->path_prefix && eh->path) {
    size_t prefix_len = strlen(eh->path_prefix);
    if (!(strncmp(display_path, eh->path_prefix, prefix_len) == 0 &&
          (display_path[prefix_len] == '/' ||
           display_path[prefix_len] == '\0'))) {
      size_t buf_len = prefix_len + 1 + strlen(display_path) + 1;
      prefixed_path = xmalloc(buf_len);
      snprintf(prefixed_path, buf_len, "%s/%s", eh->path_prefix, display_path);
      display_path = prefixed_path;
    }
  }

  char final_msg[2048];
  snprintf(final_msg, sizeof(final_msg),
           "%s%s\x1b[0m at %s:%zu:%zu: %s\n"
           "  |\n"
           "%zu | %s\n"
           "  | %s\n",
           color_prefix, level_str, display_path, loc.line, loc.col, msg_buf,
           loc.line, source_line, caret_str);
  free(prefixed_path);

  free(source_line);
  free(caret_str);

  List_push(&eh->errors, xstrdup(final_msg));
}
