#ifndef PARSER_H
#define PARSER_H

#include "tyl/ast.h"
#include "tyl/errors.h"
#include "tyl/lexer.h"
#include "tyl/utils.h"
#include <stddef.h>

typedef struct Parser Parser;
typedef struct BindingPower BindingPower;

struct Parser {
  Lexer *lexer;
  Token current_token;
  ErrorHandler *eh;
};

struct BindingPower {
  int left_bp;
  int right_bp;
};

AstNode *Parser_process(Lexer *l, ErrorHandler *eh);

#endif // !PARSER_H
