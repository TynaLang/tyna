#ifndef TYL_ERROR_H
#define TYL_ERROR_H

#include "tyl/lexer.h"
#include "tyl/utils.h"
#include <stddef.h>

typedef enum { LEVEL_ERROR, LEVEL_WARNING, LEVEL_INFO } ErrorLevel;

typedef struct ErrorHandler {
  List errors;
  const char *src;
  char *path;
  char *path_prefix;
  int has_errors;
} ErrorHandler;

void ErrorHandler_init(ErrorHandler *eh, const char *src, const char *path,
                       const char *entry_path, const char *path_prefix);
void ErrorHandler_report(ErrorHandler *eh, Location loc, const char *fmt, ...);
void ErrorHandler_report_level(ErrorHandler *eh, Location loc, ErrorLevel level,
                               const char *fmt, ...);
void ErrorHandler_show_all(ErrorHandler *eh);
void ErrorHandler_free(ErrorHandler *eh);

#endif // TYL_ERROR_H
