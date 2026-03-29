#ifndef TYL_ERROR_H
#define TYL_ERROR_H

#include "lexer.h"
#include "utils.h"
#include <stddef.h>

typedef struct ErrorHandler {
  List errors;
  const char *src;
  int has_errors;
} ErrorHandler;

void ErrorHandler_init(ErrorHandler *eh, const char *src);
void ErrorHandler_report(ErrorHandler *eh, Location loc, const char *fmt, ...);
void ErrorHandler_show_all(ErrorHandler *eh);
void ErrorHandler_free(ErrorHandler *eh);

#endif // TYL_ERROR_H
