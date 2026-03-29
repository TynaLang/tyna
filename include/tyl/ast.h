#ifndef TYL_AST_H
#define TYL_AST_H

#include "lexer.h"
#include "utils.h"
#include <stddef.h>

typedef enum AstKind AstKind;
typedef struct AstNode AstNode;
typedef enum TypeKind TypeKind;
typedef enum BinaryOp BinaryOp;
typedef enum UnaryOp UnaryOp;

enum TypeKind { TYPE_INT, TYPE_CHAR, TYPE_STRING, TYPE_UNKNOWN };

enum BinaryOp { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW };

enum UnaryOp { OP_NEG, OP_PRE_INC, OP_POST_INC, OP_PRE_DEC, OP_POST_DEC };

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
  NODE_ASSIGN,
  NODE_EXPR_STMT,
  NODE_CALL
};

struct AstNode {
  AstKind tag;
  Location loc;
  union {
    struct {
      List children;
    } program;

    struct {
      AstNode *func;
      AstNode *arg;
    } call;

    struct {
      AstNode *name;
      AstNode *value;
      TypeKind declared_type;
    } let;

    struct {
      AstNode *value;
    } print;

    struct {
      AstNode *expr;
    } expr_stmt;

    struct {
      AstNode *target;
      AstNode *value;
    } assign;

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

AstNode *AstNode_new_program(Location loc);
AstNode *AstNode_new_let(AstNode *name, AstNode *value, TypeKind type,
                         Location loc);
AstNode *AstNode_new_print(AstNode *value, Location loc);
AstNode *AstNode_new_number(double value, Location loc);
AstNode *AstNode_new_char(char value, Location loc);
AstNode *AstNode_new_string(char *value, Location loc);
AstNode *AstNode_new_ident(char *value, Location loc);
AstNode *AstNode_new_binary(AstNode *left, AstNode *right, BinaryOp op,
                            Location loc);
AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr, Location loc);
AstNode *AstNode_new_assign(AstNode *target, AstNode *value, Location loc);
AstNode *AstNode_new_expr_stmt(AstNode *expr, Location loc);
AstNode *AstNode_new_call(AstNode *func, AstNode *arg, Location loc);

AstNode *AstNode_new_assign(AstNode *target, AstNode *value, Location loc);
AstNode *AstNode_new_expr_stmt(AstNode *expr, Location loc);

void Ast_print(AstNode *node, int indent);

#endif // TYL_AST_H