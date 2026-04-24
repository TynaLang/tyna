#ifndef TYNA_AST_H
#define TYNA_AST_H

#include "tyna/lexer.h"
#include "tyna/type.h"
#include "tyna/utils.h"
#include <stddef.h>

typedef enum AstKind AstKind;
typedef struct AstNode AstNode;
typedef enum ArithmOp ArithmOp;
typedef enum CompareOp CompareOp;
typedef enum EqualityOp EqualityOp;
typedef enum LogicalOp LogicalOp;
typedef enum UnaryOp UnaryOp;

enum ArithmOp { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_MOD };
enum CompareOp { OP_LT, OP_GT, OP_LE, OP_GE };
enum EqualityOp { OP_EQ, OP_NE };
enum LogicalOp { OP_AND, OP_OR };
enum UnaryOp {
  OP_NEG,
  OP_PRE_INC,
  OP_POST_INC,
  OP_PRE_DEC,
  OP_POST_DEC,
  OP_NOT,
  OP_ADDR_OF,
  OP_DEREF
};

enum AstKind {
  NODE_AST_ROOT,
  NODE_VAR_DECL,
  NODE_PRINT_STMT,

  NODE_NUMBER,
  NODE_CHAR,
  NODE_BOOL,
  NODE_STRING,
  NODE_NULL,
  NODE_VAR,

  NODE_BINARY_ARITH,
  NODE_BINARY_COMPARE,
  NODE_BINARY_EQUALITY,
  NODE_BINARY_LOGICAL,
  NODE_BINARY_IS,
  NODE_BINARY_ELSE,

  NODE_UNARY,

  NODE_CAST_EXPR,
  NODE_ASSIGN_EXPR,
  NODE_EXPR_STMT,

  NODE_TERNARY,

  NODE_CALL,
  NODE_FUNC_DECL,
  NODE_RETURN_STMT,
  NODE_PARAM,
  NODE_BLOCK,

  NODE_FIELD,
  NODE_INDEX,
  NODE_STATIC_MEMBER,
  NODE_STRUCT_DECL,
  NODE_UNION_DECL,
  NODE_ERROR_DECL,
  NODE_ERROR_SET_DECL,
  NODE_IMPL_DECL,
  NODE_IMPORT,
  NODE_NEW_EXPR,

  NODE_ARRAY_LITERAL,
  NODE_ARRAY_REPEAT,

  NODE_IF_STMT,
  NODE_SWITCH_STMT,
  NODE_CASE,
  NODE_LOOP_STMT,
  NODE_WHILE_STMT,
  NODE_FOR_STMT,
  NODE_FOR_IN_STMT,

  NODE_BREAK,
  NODE_CONTINUE,

  NODE_DEFER,
  NODE_INTRINSIC_COMPARE,
};

struct AstNode {
  AstKind tag;
  Location loc;
  Type *resolved_type;
  union {
    struct {
      List items; // List<AstNode*>
    } array_literal;

    struct {
      AstNode *value;
      AstNode *count;
    } array_repeat;

    struct {
      AstNode *left;
      AstNode *right;
      EqualityOp op;
    } intrinsic_compare;

    struct {
      AstNode *name;
      List members;      // List<AstNode* (NODE_VAR_DECL or NODE_FUNC_DECL)>
      List placeholders; // List<StringView*>
      bool is_frozen;
      bool is_export;
    } struct_decl;

    struct {
      AstNode *name;
      List members;      // List<AstNode* (NODE_VAR_DECL or NODE_FUNC_DECL)>
      List placeholders; // List<StringView*>
      bool is_frozen;
      bool is_export;
    } union_decl;

    struct {
      AstNode *name;
      List members; // List<AstNode* (NODE_VAR_DECL)>
      StringView message;
      bool is_export;
    } error_decl;

    struct {
      AstNode *name;
      List members; // List<AstNode* (NODE_VAR)>
      bool is_export;
    } error_set_decl;

    struct {
      Type *type;
      List members; // List<AstNode* (NODE_FUNC_DECL)>
    } impl_decl;

    struct {
      AstNode *name;
      List params;
      AstNode *body;
      Type *return_type;
      bool is_static;
      bool is_export;
      bool is_pub_module;
      bool is_external;
      bool requires_arena;
      bool consumes_string_arg;
    } func_decl;

    struct {
      List children;
    } ast_root;

    struct {
      AstNode *expr;
      Type *target_type;
    } cast_expr;

    struct {
      AstNode *name;
      AstNode *value;
      Type *declared_type;
      int is_const;
      bool is_export;
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
      AstNode *condition;
      AstNode *true_expr;
      AstNode *false_expr;
    } ternary;

    struct {
      double value;
      StringView raw_text;
    } number;

    struct {
      int value;
    } boolean;

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
      ArithmOp op;
    } binary_arith;

    struct {
      AstNode *left;
      AstNode *right;
      CompareOp op;
    } binary_compare;

    struct {
      AstNode *left;
      AstNode *right;
      EqualityOp op;
    } binary_equality;

    struct {
      AstNode *left;
      AstNode *right;
      LogicalOp op;
    } binary_logical;

    struct {
      AstNode *expr;
      UnaryOp op;
    } unary;

    struct {
      AstNode *func;
      List args;         // List<AstNode*> for call arguments
      List generic_args; // List<Type*> for generic type arguments
    } call;

    struct {
      AstNode *left;
      AstNode *right;
    } binary_is;

    struct {
      AstNode *left;
      AstNode *right;
    } binary_else;

    struct {
      Type *target_type;
      List args;        // List<AstNode*>
      List field_inits; // List<AstNode* (NODE_ASSIGN_EXPR with var target)>
    } new_expr;

    struct {
      AstNode *expr; // Optional
    } return_stmt;

    struct {
      AstNode *name;
      Type *type;
      AstNode *default_value;
      bool requires_storage;
    } param;

    struct {
      List statements;
    } block;

    struct {
      AstNode *object;
      StringView field;
    } field;

    struct {
      StringView parent;
      StringView member;
    } static_member;

    struct {
      AstNode *array;
      AstNode *index;
    } index;

    struct {
      AstNode *expr;
    } defer;

    struct {
      AstNode *expr;
    } loop;

    struct {
      AstNode *condition;
      AstNode *body;
    } while_stmt;

    struct {
      AstNode *init;
      AstNode *condition;
      AstNode *increment;
      AstNode *body;
    } for_stmt;

    struct {
      AstNode *var;
      AstNode *iterable;
      AstNode *body;
    } for_in_stmt;

    struct {
      AstNode *condition;
      AstNode *then_branch;
      AstNode *else_branch;
    } if_stmt;

    struct {
      AstNode *expr;
      List cases; // List<AstNode*> for NODE_CASE
    } switch_stmt;

    struct {
      AstNode *pattern;   // NODE_STRING or NODE_VAR
      Type *pattern_type; // variant type for union cases
      AstNode *body;
    } case_stmt;

    struct {
      StringView path;
      StringView alias;
    } import;
  };
};

AstNode *AstNode_new_program(Location loc);
AstNode *AstNode_new_import(StringView path, StringView alias, Location loc);

AstNode *AstNode_new_var_decl(AstNode *name, AstNode *value, Type *type,
                              int is_const, bool is_export, Location loc);
AstNode *AstNode_new_print_stmt(List values, Location loc);

AstNode *AstNode_new_number(double value, StringView raw_text, Location loc);
AstNode *AstNode_new_char(char value, Location loc);
AstNode *AstNode_new_bool(int value, Location loc);
AstNode *AstNode_new_string(StringView value, Location loc);
AstNode *AstNode_new_null(Location loc);
AstNode *AstNode_new_new_expr(Type *target_type, List args, List field_inits,
                              Location loc);

AstNode *AstNode_new_var(StringView value, Location loc);
AstNode *AstNode_new_array_literal(List items, Location loc);

AstNode *AstNode_new_binary_arith(AstNode *left, AstNode *right, ArithmOp op,
                                  Location loc);
AstNode *AstNode_new_binary_compare(AstNode *left, AstNode *right, CompareOp op,
                                    Location loc);
AstNode *AstNode_new_binary_equality(AstNode *left, AstNode *right,
                                     EqualityOp op, Location loc);
AstNode *AstNode_new_binary_logical(AstNode *left, AstNode *right, LogicalOp op,
                                    Location loc);
AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr, Location loc);

AstNode *AstNode_new_cast_expr(AstNode *expr, Type *type, Location loc);
AstNode *AstNode_new_assign_expr(AstNode *target, AstNode *value, Location loc);
AstNode *AstNode_new_expr_stmt(AstNode *expr, Location loc);

AstNode *AstNode_new_call(AstNode *func, List args, Location loc);
AstNode *AstNode_new_func_decl(AstNode *name, List params, Type *ret_type,
                               AstNode *body, bool is_static, bool is_export,
                               bool is_pub_module, bool is_external,
                               Location loc);
AstNode *AstNode_new_return(AstNode *expr, Location loc);
AstNode *AstNode_new_param(AstNode *name, Type *type, Location loc);
AstNode *AstNode_new_block(Location loc);

AstNode *AstNode_new_index(AstNode *expr, AstNode *index, Location loc);
AstNode *AstNode_new_field(AstNode *expr, StringView field, Location loc);
AstNode *AstNode_new_static_member(StringView parent, StringView member,
                                   Location loc);
AstNode *AstNode_new_array_repeat(AstNode *value, AstNode *count, Location loc);

AstNode *AstNode_new_if_stmt(AstNode *condition, AstNode *then_branch,
                             AstNode *else_branch, Location loc);
AstNode *AstNode_new_switch_stmt(AstNode *expr, List cases, Location loc);
AstNode *AstNode_new_case_stmt(AstNode *pattern, Type *pattern_type,
                               AstNode *body, Location loc);
AstNode *AstNode_new_defer(AstNode *expr, Location loc);
AstNode *AstNode_new_loop(AstNode *expr, Location loc);
AstNode *AstNode_new_while(AstNode *condition, AstNode *body, Location loc);
AstNode *AstNode_new_for(AstNode *init, AstNode *cond, AstNode *inc,
                         AstNode *body, Location loc);
AstNode *AstNode_new_for_in(AstNode *var, AstNode *iterable, AstNode *body,
                            Location loc);
AstNode *AstNode_new_break(Location loc);
AstNode *AstNode_new_continue(Location loc);

AstNode *AstNode_new_ternary(AstNode *condition, AstNode *true_expr,
                             AstNode *false_expr, Location loc);

AstNode *AstNode_new_struct_decl(AstNode *name, List members, List placeholders,
                                 bool is_frozen, bool is_export, Location loc);
AstNode *AstNode_new_union_decl(AstNode *name, List members, List placeholders,
                                bool is_frozen, bool is_export, Location loc);
AstNode *AstNode_new_error_decl(AstNode *name, List members, StringView message,
                                bool is_export, Location loc);
AstNode *AstNode_new_error_set_decl(AstNode *name, List members, bool is_export,
                                    Location loc);
AstNode *AstNode_new_impl_decl(Type *type, List members, Location loc);
AstNode *AstNode_new_array_literal(List elements, Location loc);
AstNode *AstNode_new_binary_is(AstNode *left, AstNode *right, Location loc);
AstNode *AstNode_new_binary_else(AstNode *left, AstNode *right, Location loc);
AstNode *Ast_clone(AstNode *node);

PrimitiveKind Token_token_to_type(TokenType t);

void Ast_free(AstNode *node);
void Ast_print(AstNode *node, int indent);
void Ast_print_to_stream(FILE *out, AstNode *node, int indent);

#endif // TYNA_AST_H
