#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"
#include "utils.h"
#include <stddef.h>

typedef struct Parser Parser;
typedef struct BindingPower BindingPower;

struct Parser {
  Lexer *lexer;
  Token current_token;
};

struct BindingPower {
  int left_bp;
  int right_bp;
};

AstNode *Parser_process(Lexer *l);

#endif // !PARSER_H
