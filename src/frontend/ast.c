#include "ast.h"
#include "semantic.h"
#include <stdio.h>
#include <stdlib.h>

static AstNode *AstNode_new(AstKind ast_kind, Location loc) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node) {
    fprintf(stderr, "Failed to allocate AST node\n");
    exit(1);
  }
  node->tag = ast_kind;
  node->loc = loc;
  return node;
}

AstNode *AstNode_new_program(Location loc) {
  AstNode *node = AstNode_new(NODE_PROGRAM, loc);
  List_init(&node->program.children);
  return node;
}

AstNode *AstNode_new_var_decl(AstNode *name, AstNode *value, TypeKind type,
                              int is_const, Location loc) {
  AstNode *node = AstNode_new(NODE_VAR_DECL, loc);
  node->var_decl.name = name;
  node->var_decl.value = value;
  node->var_decl.declared_type = type;
  node->var_decl.is_const = is_const;
  return node;
}

AstNode *AstNode_new_print_stmt(List values, Location loc) {
  AstNode *node = AstNode_new(NODE_PRINT_STMT, loc);
  node->print_stmt.values = values;
  return node;
}

AstNode *AstNode_new_number(double value, StringView raw_text, Location loc) {
  AstNode *node = AstNode_new(NODE_NUMBER, loc);
  node->number.value = value;
  node->number.raw_text = raw_text;
  return node;
}

AstNode *AstNode_new_char(char value, Location loc) {
  AstNode *node = AstNode_new(NODE_CHAR, loc);
  node->char_lit.value = value;
  return node;
}

AstNode *AstNode_new_string(StringView value, Location loc) {
  AstNode *node = AstNode_new(NODE_STRING, loc);
  node->string.value = value;
  return node;
}

AstNode *AstNode_new_var(StringView value, Location loc) {
  AstNode *node = AstNode_new(NODE_VAR, loc);
  node->var.value = value;
  return node;
}

AstNode *AstNode_new_binary(AstNode *left, AstNode *right, BinaryOp op,
                            Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY, loc);
  node->binary.left = left;
  node->binary.right = right;
  node->binary.op = op;
  return node;
}

AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_UNARY, loc);
  node->unary.expr = expr;
  node->unary.op = op;
  return node;
}

AstNode *AstNode_new_cast_expr(AstNode *expr, TypeKind type, Location loc) {
  AstNode *node = AstNode_new(NODE_CAST_EXPR, loc);
  node->cast_expr.expr = expr;
  node->cast_expr.target_type = type;
  return node;
}

AstNode *AstNode_new_assign_expr(AstNode *target, AstNode *value,
                                 Location loc) {
  AstNode *node = AstNode_new(NODE_ASSIGN_EXPR, loc);
  node->assign_expr.target = target;
  node->assign_expr.value = value;
  return node;
}

AstNode *AstNode_new_expr_stmt(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_EXPR_STMT, loc);
  node->expr_stmt.expr = expr;
  return node;
}

AstNode *AstNode_new_call(AstNode *func, List args, Location loc) {
  AstNode *node = AstNode_new(NODE_CALL, loc);
  node->call.func = func;
  node->call.args = args;
  return node;
}

AstNode *AstNode_new_func_decl(StringView name, List params, TypeKind ret_type,
                               AstNode *body, Location loc) {
  AstNode *node = AstNode_new(NODE_FUNC_DECL, loc);
  node->func_decl.name = name;
  node->func_decl.params = params;
  node->func_decl.return_type = ret_type;
  node->func_decl.body = body;
  return node;
}

AstNode *AstNode_new_return(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_RETURN_STMT, loc);
  node->return_stmt.expr = expr;
  return node;
}

AstNode *AstNode_new_param(StringView name, TypeKind type, Location loc) {
  AstNode *node = AstNode_new(NODE_PARAM, loc);
  node->param.name = name;
  node->param.type = type;
  return node;
}

AstNode *AstNode_new_index(AstNode *expr, AstNode *index, Location loc) {
  AstNode *node = AstNode_new(NODE_FIELD, loc);
  node->index.array = expr;
  node->index.index = index;
  return node;
}

AstNode *AstNode_new_field(AstNode *expr, StringView field, Location loc) {
  AstNode *node = AstNode_new(NODE_FIELD, loc);
  node->field.object = expr;
  node->field.field = field;
  return node;
}

TypeKind Token_token_to_type(TokenType t) {
  switch (t) {
  case TOKEN_TYPE_INT:
  case TOKEN_TYPE_I32:
    return TYPE_I32;
  case TOKEN_TYPE_I8:
    return TYPE_I8;
  case TOKEN_TYPE_I16:
    return TYPE_I16;
  case TOKEN_TYPE_I64:
    return TYPE_I64;
  case TOKEN_TYPE_U8:
    return TYPE_U8;
  case TOKEN_TYPE_U16:
    return TYPE_U16;
  case TOKEN_TYPE_U32:
    return TYPE_U32;
  case TOKEN_TYPE_U64:
    return TYPE_U64;
  case TOKEN_TYPE_FLOAT:
  case TOKEN_TYPE_F32:
    return TYPE_F32;
  case TOKEN_TYPE_F64:
    return TYPE_F64;
  case TOKEN_TYPE_CHAR:
    return TYPE_CHAR;
  case TOKEN_TYPE_STR:
    return TYPE_STRING;
  case TOKEN_TYPE_VOID:
    return TYPE_VOID;
  default:
    return TYPE_UNKNOWN;
  }
}

static void print_indent(int indent) {
  for (int i = 0; i < indent; i++) {
    printf("  ");
  }
}

void Ast_print(AstNode *node, int indent) {
  if (!node)
    return;

  print_indent(indent);

  switch (node->tag) {
  case NODE_PROGRAM:
    printf("PROGRAM\n");
    for (size_t i = 0; i < node->program.children.len; i++) {
      AstNode *child = node->program.children.items[i];
      Ast_print(child, indent + 1);
    }
    break;
  case NODE_VAR_DECL:
    printf("%s " SV_FMT " : %d\n", node->var_decl.is_const ? "CONST" : "LET",
           SV_ARG(node->var_decl.name->var.value),
           node->var_decl.declared_type);
    print_indent(indent + 1);
    printf("VALUE:\n");
    Ast_print(node->var_decl.value, indent + 2);
    break;
  case NODE_PRINT_STMT:
    printf("PRINT_STMT\n");
    for (size_t i = 0; i < node->print_stmt.values.len; i++) {
      AstNode *val = node->print_stmt.values.items[i];
      Ast_print(val, indent + 1);
    }
    break;
  case NODE_NUMBER:
    printf("NUMBER: %f (raw: " SV_FMT ")\n", node->number.value,
           SV_ARG(node->number.raw_text));
    break;
  case NODE_CHAR:
    printf("CHAR: %c\n", node->char_lit.value);
    break;
  case NODE_STRING:
    printf("STRING: " SV_FMT "\n", SV_ARG(node->string.value));
    break;
  case NODE_VAR:
    printf("VAR: " SV_FMT "\n", SV_ARG(node->var.value));
    break;
  case NODE_BINARY:
    printf("BINARY OP: %d\n", node->binary.op);
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary.right, indent + 2);
    break;
  case NODE_UNARY:
    printf("UNARY OP: %d\n", node->unary.op);
    print_indent(indent + 1);
    printf("EXPR:\n");
    Ast_print(node->unary.expr, indent + 2);
    break;
  case NODE_CAST_EXPR:
    printf("CAST TO %s\n", type_to_name(node->cast_expr.target_type));
    Ast_print(node->cast_expr.expr, indent + 1);
    break;
  case NODE_CALL:
    printf("CALL:\n");
    print_indent(indent + 1);
    printf("FUNC:\n");
    Ast_print(node->call.func, indent + 2);
    print_indent(indent + 1);
    printf("ARG:\n");
    for (size_t i = 0; i < node->call.args.len; i++) {
      AstNode *arg = node->call.args.items[i];
      Ast_print(arg, indent + 2);
    }
    break;
  case NODE_FUNC_DECL:
    printf("FUNC DECL: " SV_FMT " RETURNS %s\n", SV_ARG(node->func_decl.name),
           type_to_name(node->func_decl.return_type));
    print_indent(indent + 1);
    printf("PARAMS:\n");
    for (size_t i = 0; i < node->func_decl.params.len; i++) {
      AstNode *param = node->func_decl.params.items[i];
      Ast_print(param, indent + 2);
    }
    print_indent(indent + 1);
    printf("BODY:\n");
    Ast_print(node->func_decl.body, indent + 2);
    break;
  case NODE_RETURN_STMT:
    printf("RETURN_STMT\n");
    if (node->return_stmt.expr) {
      print_indent(indent + 1);
      printf("EXPR:\n");
      Ast_print(node->return_stmt.expr, indent + 2);
    }
    break;
  case NODE_PARAM:
    printf("PARAM: " SV_FMT " : %s\n", SV_ARG(node->param.name),
           type_to_name(node->param.type));
    break;
  case NODE_EXPR_STMT:
    printf("EXPR_STMT\n");
    Ast_print(node->expr_stmt.expr, indent + 1);
    break;
  case NODE_ASSIGN_EXPR:
    printf("ASSIGN_EXPR\n");
    print_indent(indent + 1);
    printf("TARGET:\n");
    Ast_print(node->assign_expr.target, indent + 2);
    print_indent(indent + 1);
    printf("VALUE:\n");
    Ast_print(node->assign_expr.value, indent + 2);
    break;
  default:
    printf("UNKNOWN NODE\n");
    break;
  }
}
