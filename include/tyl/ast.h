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

enum TypeKind {
  TYPE_I8,
  TYPE_I16,
  TYPE_I32,
  TYPE_I64,
  TYPE_U8,
  TYPE_U16,
  TYPE_U32,
  TYPE_U64,
  TYPE_F32,
  TYPE_F64,
  TYPE_CHAR,
  TYPE_STRING,
  TYPE_UNKNOWN
};

enum BinaryOp { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW };

enum UnaryOp { OP_NEG, OP_PRE_INC, OP_POST_INC, OP_PRE_DEC, OP_POST_DEC };

enum AstKind {
  NODE_PROGRAM,
  NODE_VAR_DECL,
  NODE_PRINT_STMT,
  NODE_NUMBER,
  NODE_CHAR,
  NODE_STRING,
  NODE_VAR,
  NODE_BINARY,
  NODE_UNARY,
  NODE_CAST_EXPR,
  NODE_ASSIGN_EXPR,
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
      AstNode *expr;
      TypeKind target_type;
    } cast_expr;

    struct {
      AstNode *func;
      AstNode *arg;
    } call;

    struct {
      AstNode *name;
      AstNode *value;
      TypeKind declared_type;
      int is_const;
    } var_decl;

    struct {
      List values;
    } print_stmt;

    struct {
      AstNode *expr;
    } expr_stmt;

    struct {
      AstNode *target;
      AstNode *value;
    } assign_expr;

    struct {
      double value;
      StringView raw_text;
    } number;

    struct {
      char value;
    } char_lit;

    struct {
      StringView value;
    } string;

    struct {
      StringView value;
    } var;

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
AstNode *AstNode_new_var_decl(AstNode *name, AstNode *value, TypeKind type,
                              int is_const, Location loc);
AstNode *AstNode_new_print_stmt(List values, Location loc);
AstNode *AstNode_new_number(double value, StringView raw_text, Location loc);
AstNode *AstNode_new_char(char value, Location loc);
AstNode *AstNode_new_string(StringView value, Location loc);
AstNode *AstNode_new_var(StringView value, Location loc);
AstNode *AstNode_new_binary(AstNode *left, AstNode *right, BinaryOp op,
                            Location loc);
AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr, Location loc);
AstNode *AstNode_new_cast_expr(AstNode *expr, TypeKind type, Location loc);
AstNode *AstNode_new_assign_expr(AstNode *target, AstNode *value, Location loc);
AstNode *AstNode_new_expr_stmt(AstNode *expr, Location loc);
AstNode *AstNode_new_call(AstNode *func, AstNode *arg, Location loc);

TypeKind Token_token_to_type(TokenType t);

void Ast_print(AstNode *node, int indent);

#endif // TYL_AST_H