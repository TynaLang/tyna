#ifndef PARSER_H
#define PARSER_H

#include "tyna/ast.h"
#include "tyna/errors.h"
#include "tyna/lexer.h"
#include "tyna/utils.h"
#include <stddef.h>

typedef struct Parser Parser;
typedef struct BindingPower BindingPower;

struct Parser {
  Lexer *lexer;
  Token current_token;
  ErrorHandler *eh;
  TypeContext *type_ctx;
  List type_placeholders;       // active generic placeholder types
  List placeholder_scope_stack; // stack of placeholder list lengths for nested
                                // scopes
  bool placeholder_mode;
};

struct BindingPower {
  int left_bp;
  int right_bp;
};

AstNode *parser_process(Lexer *l, ErrorHandler *eh, TypeContext *type_ctx);

#endif // !PARSER_H
