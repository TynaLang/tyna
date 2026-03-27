#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "utils.h"
#include <stddef.h>

typedef struct Parser Parser;
typedef enum TypeKind TypeKind;
typedef enum AstKind AstKind;
typedef struct AstNode AstNode;
typedef enum BinaryOp BinaryOp;
typedef enum UnaryOp UnaryOp;
typedef struct BindingPower BindingPower;

struct Parser {
  Lexer *lexer;
  Token current_token;
};

enum TypeKind { TYPE_INT, TYPE_CHAR, TYPE_STRING, TYPE_UNKNOWN };
enum BinaryOp {
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_POW,
};

enum UnaryOp {
  OP_NEG,
  OP_PRE_INC,
  OP_POST_INC,
  OP_PRE_DEC,
  OP_POST_DEC,
};

enum AstKind {
  NODE_PROGRAM,

  NODE_LET,
  NODE_PRINT,

  NODE_NUMBER,
  NODE_CHAR,
  NODE_STRING,
  NODE_IDENT,

  NODE_BINARY,
  NODE_UNARY,
};

struct BindingPower {
  int left_bp;
  int right_bp;
};

struct AstNode {
  AstKind tag;
  union {
    struct {
      List children;
    } program;

    struct {
      AstNode *name;
      AstNode *value;
      TypeKind declared_type;
    } let;
    struct {
      AstNode *value;
    } print;

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
    } ident;

    struct {
      AstNode *left;
      AstNode *right;
      BinaryOp op;
    } binary;

    struct {
      AstNode *expr;
      UnaryOp op;
    } unary;
  };
};

AstNode *Parser_process(Lexer *lexer);
void Ast_print(AstNode *node, int indent);
#endif // !PARSER_H
