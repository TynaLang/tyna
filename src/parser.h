#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "utils.h"
#include <stddef.h>

typedef struct Parser Parser;
typedef enum TypeKind TypeKind;
typedef enum AstKind AstKind;
typedef struct AstNode AstNode;

struct Parser {
  Lexer *lexer;
  Token current_token;
};

enum TypeKind { TYPE_INT, TYPE_CHAR, TYPE_STRING, TYPE_UNKNOWN };

enum AstKind {
  NODE_PROGRAM,
  NODE_STATEMENT,
  NODE_EXPR,
  NODE_LET,
  NODE_PRINT,
  NODE_NUMBER,
  NODE_CHAR,
  NODE_STRING,
  NODE_IDENT,
};

struct AstNode {
  AstKind tag;
  union {
    struct {
      List children;
    } program;

    struct {
      double value;
    } number;

    struct {
      char value;
    } char_lit;

    struct {
      char *value;
    } string;

    struct {
      char *value;
    } literal;

    struct {
      char *value;
    } ident;

    struct {
      AstNode *name;
      AstNode *value;
      TypeKind declared_type;
    } let;

    struct {
      AstNode *value;
    } print;
  };
};

AstNode *Parser_process(Lexer *lexer);

#endif // !PARSER_H
