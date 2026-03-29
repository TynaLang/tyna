#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "lexer.h"

void ErrorHandler_init(ErrorHandler *eh, const char *src) {
  List_init(&eh->errors);
  eh->src = src;
  eh->has_errors = 0;
}

void ErrorHandler_free(ErrorHandler *eh) {
  for (size_t i = 0; i < eh->errors.len; i++) {
    free(eh->errors.items[i]);
  }
}

void ErrorHandler_show_all(ErrorHandler *eh) {
  if (!eh->has_errors)
    return;
  for (size_t i = 0; i < eh->errors.len; i++) {
    fprintf(stderr, "%s\n", (char *)eh->errors.items[i]);
  }
}

void ErrorHandler_report(ErrorHandler *eh, Location loc, const char *fmt, ...) {
  eh->has_errors = 1;

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
  char *source_line = malloc(line_len + 1);
  if (source_line) {
    strncpy(source_line, line_start, line_len);
    source_line[line_len] = '\0';
  } else {
    source_line = strdup("");
  }

  // Create the col pointer `   ^`
  size_t caret_pad = (loc.col > 1) ? loc.col - 1 : 0;
  if (caret_pad > line_len)
    caret_pad = line_len; // Cap at line length

  char *caret_str = malloc(caret_pad + 2); // + '^' + '\0'
  if (caret_str) {
    memset(caret_str, ' ', caret_pad);
    caret_str[caret_pad] = '^';
    caret_str[caret_pad + 1] = '\0';
  } else {
    caret_str = strdup("^");
  }

  char final_msg[2048];
  // ANSI colors: \x1b[31;1m for bold red, \x1b[0m to reset
  snprintf(final_msg, sizeof(final_msg),
           "\x1b[31;1mError\x1b[0m at line %zu, col %zu: %s\n"
           "  |\n"
           "%zu | %s\n"
           "  | %s\n",
           loc.line, loc.col, msg_buf, loc.line, source_line, caret_str);

  free(source_line);
  free(caret_str);

  List_push(&eh->errors, strdup(final_msg));
}
