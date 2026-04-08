#include <stdarg.h>

#include "sema_internal.h"

void sema_error(Sema *s, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report(s->eh, n->loc, "%s", msg_buf);
}

void sema_error_at(Sema *s, Location loc, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report(s->eh, loc, "%s", msg_buf);
}

void sema_warning(Sema *s, AstNode *n, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report_level(s->eh, n->loc, LEVEL_WARNING, "%s", msg_buf);
}

void sema_warning_at(Sema *s, Location loc, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char msg_buf[1024];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  ErrorHandler_report_level(s->eh, loc, LEVEL_WARNING, "%s", msg_buf);
}