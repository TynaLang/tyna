#include "tyl/ast.h"
#include "tyl/semantic.h"
#include <stdio.h>
#include <stdlib.h>

static AstNode *AstNode_new(AstKind ast_kind, Location loc) {
  AstNode *node = xmalloc(sizeof(AstNode));
  if (!node) {
    fprintf(stderr, "Failed to allocate AST node\n");
    exit(1);
  }
  node->tag = ast_kind;
  node->loc = loc;
  node->resolved_type = NULL;
  return node;
}

AstNode *AstNode_new_program(Location loc) {
  AstNode *node = AstNode_new(NODE_AST_ROOT, loc);
  List_init(&node->ast_root.children);
  return node;
}

AstNode *AstNode_new_var_decl(AstNode *name, AstNode *value, Type *type,
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

AstNode *AstNode_new_bool(int value, Location loc) {
  AstNode *node = AstNode_new(NODE_BOOL, loc);
  node->boolean.value = value;
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

AstNode *AstNode_new_array_literal(List items, Location loc) {
  AstNode *node = AstNode_new(NODE_ARRAY_LITERAL, loc);
  node->array_literal.items = items;
  return node;
}

AstNode *AstNode_new_binary_arith(AstNode *left, AstNode *right, ArithmOp op,
                                  Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_ARITH, loc);
  node->binary_arith.left = left;
  node->binary_arith.right = right;
  node->binary_arith.op = op;
  return node;
}

AstNode *AstNode_new_binary_compare(AstNode *left, AstNode *right, CompareOp op,
                                    Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_COMPARE, loc);
  node->binary_compare.left = left;
  node->binary_compare.right = right;
  node->binary_compare.op = op;
  return node;
}

AstNode *AstNode_new_binary_equality(AstNode *left, AstNode *right,
                                     EqualityOp op, Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_EQUALITY, loc);
  node->binary_equality.left = left;
  node->binary_equality.right = right;
  node->binary_equality.op = op;
  return node;
}

AstNode *AstNode_new_binary_logical(AstNode *left, AstNode *right, LogicalOp op,
                                    Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY_LOGICAL, loc);
  node->binary_logical.left = left;
  node->binary_logical.right = right;
  node->binary_logical.op = op;
  return node;
}

AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_UNARY, loc);
  node->unary.expr = expr;
  node->unary.op = op;
  return node;
}

AstNode *AstNode_new_cast_expr(AstNode *expr, Type *type, Location loc) {
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

AstNode *AstNode_new_func_decl(StringView name, List params, Type *ret_type,
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

AstNode *AstNode_new_param(StringView name, Type *type, Location loc) {
  AstNode *node = AstNode_new(NODE_PARAM, loc);
  node->param.name = name;
  node->param.type = type;
  return node;
}

AstNode *AstNode_new_block(Location loc) {
  AstNode *node = AstNode_new(NODE_BLOCK, loc);
  List_init(&node->block.statements);
  return node;
}

AstNode *AstNode_new_if_stmt(AstNode *condition, AstNode *then_branch,
                             AstNode *else_branch, Location loc) {
  AstNode *node = AstNode_new(NODE_IF_STMT, loc);
  node->if_stmt.condition = condition;
  node->if_stmt.then_branch = then_branch;
  node->if_stmt.else_branch = else_branch;
  return node;
}

AstNode *AstNode_new_defer(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_DEFER, loc);
  node->defer.expr = expr;
  return node;
}

AstNode *AstNode_new_ternary(AstNode *condition, AstNode *true_expr,
                             AstNode *false_expr, Location loc) {
  AstNode *node = AstNode_new(NODE_TERNARY, loc);
  node->ternary.condition = condition;
  node->ternary.true_expr = true_expr;
  node->ternary.false_expr = false_expr;
  return node;
}

AstNode *AstNode_new_struct_decl(AstNode *name, List members, Location loc) {
  AstNode *node = AstNode_new(NODE_STRUCT_DECL, loc);
  node->struct_decl.name = name;
  node->struct_decl.members = members;
  return node;
}

AstNode *AstNode_new_index(AstNode *expr, AstNode *index, Location loc) {
  AstNode *node = AstNode_new(NODE_INDEX, loc);
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

AstNode *AstNode_new_static_member(StringView parent, StringView member,
                                   Location loc) {
  AstNode *node = AstNode_new(NODE_STATIC_MEMBER, loc);
  node->static_member.parent = parent;
  node->static_member.member = member;
  return node;
}

AstNode *AstNode_new_array_repeat(AstNode *value, AstNode *count,
                                  Location loc) {
  AstNode *node = AstNode_new(NODE_ARRAY_REPEAT, loc);
  node->array_repeat.value = value;
  node->array_repeat.count = count;
  return node;
}

void Ast_free(AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_AST_ROOT:
    for (size_t i = 0; i < node->ast_root.children.len; i++) {
      Ast_free((AstNode *)node->ast_root.children.items[i]);
    }
    List_free(&node->ast_root.children, 0);
    break;
  case NODE_VAR_DECL:
    Ast_free(node->var_decl.name);
    Ast_free(node->var_decl.value);
    break;
  case NODE_PRINT_STMT:
    for (size_t i = 0; i < node->print_stmt.values.len; i++) {
      Ast_free((AstNode *)node->print_stmt.values.items[i]);
    }
    List_free(&node->print_stmt.values, 0);
    break;
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_BOOL:
  case NODE_STRING:
  case NODE_VAR:
    break;
  case NODE_BINARY_ARITH:
    Ast_free(node->binary_arith.left);
    Ast_free(node->binary_arith.right);
    break;
  case NODE_BINARY_COMPARE:
    Ast_free(node->binary_compare.left);
    Ast_free(node->binary_compare.right);
    break;
  case NODE_BINARY_EQUALITY:
    Ast_free(node->binary_equality.left);
    Ast_free(node->binary_equality.right);
    break;
  case NODE_BINARY_LOGICAL:
    Ast_free(node->binary_logical.left);
    Ast_free(node->binary_logical.right);
    break;
  case NODE_UNARY:
    Ast_free(node->unary.expr);
    break;
  case NODE_CAST_EXPR:
    Ast_free(node->cast_expr.expr);
    break;
  case NODE_ASSIGN_EXPR:
    Ast_free(node->assign_expr.target);
    Ast_free(node->assign_expr.value);
    break;
  case NODE_EXPR_STMT:
    Ast_free(node->expr_stmt.expr);
    break;
  case NODE_TERNARY:
    Ast_free(node->ternary.condition);
    Ast_free(node->ternary.true_expr);
    Ast_free(node->ternary.false_expr);
    break;
  case NODE_CALL:
    Ast_free(node->call.func);
    for (size_t i = 0; i < node->call.args.len; i++) {
      Ast_free((AstNode *)node->call.args.items[i]);
    }
    List_free(&node->call.args, 0);
    break;
  case NODE_FUNC_DECL:
    for (size_t i = 0; i < node->func_decl.params.len; i++) {
      Ast_free((AstNode *)node->func_decl.params.items[i]);
    }
    List_free(&node->func_decl.params, 0);
    Ast_free(node->func_decl.body);
    break;
  case NODE_RETURN_STMT:
    Ast_free(node->return_stmt.expr);
    break;
  case NODE_PARAM:
    break;
  case NODE_BLOCK:
    for (size_t i = 0; i < node->block.statements.len; i++) {
      Ast_free((AstNode *)node->block.statements.items[i]);
    }
    List_free(&node->block.statements, 0);
    break;
  case NODE_IF_STMT:
    Ast_free(node->if_stmt.condition);
    Ast_free(node->if_stmt.then_branch);
    Ast_free(node->if_stmt.else_branch);
    break;
  case NODE_DEFER:
    Ast_free(node->defer.expr);
    break;
  case NODE_ARRAY_REPEAT:
    Ast_free(node->array_repeat.value);
    Ast_free(node->array_repeat.count);
    break;
  }
  free(node);
}

PrimitiveKind Token_token_to_type(TokenType t) {
  switch (t) {
  case TOKEN_TYPE_INT:
  case TOKEN_TYPE_I32:
    return PRIM_I32;
  case TOKEN_TYPE_I8:
    return PRIM_I8;
  case TOKEN_TYPE_I16:
    return PRIM_I16;
  case TOKEN_TYPE_I64:
    return PRIM_I64;
  case TOKEN_TYPE_U8:
    return PRIM_U8;
  case TOKEN_TYPE_U16:
    return PRIM_U16;
  case TOKEN_TYPE_U32:
    return PRIM_U32;
  case TOKEN_TYPE_U64:
    return PRIM_U64;
  case TOKEN_TYPE_FLOAT:
  case TOKEN_TYPE_F32:
    return PRIM_F32;
  case TOKEN_TYPE_F64:
    return PRIM_F64;
  case TOKEN_TYPE_BOOLEAN:
    return PRIM_BOOL;
  case TOKEN_TYPE_CHAR:
    return PRIM_CHAR;
  case TOKEN_TYPE_STR:
    return PRIM_STRING;
  case TOKEN_TYPE_VOID:
    return PRIM_VOID;
  default:
    return PRIM_UNKNOWN;
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
  case NODE_AST_ROOT:
    printf("PROGRAM\n");
    for (size_t i = 0; i < node->ast_root.children.len; i++) {
      AstNode *child = node->ast_root.children.items[i];
      Ast_print(child, indent + 1);
    }
    break;
  case NODE_VAR_DECL:
    printf("%s " SV_FMT " : %s\n", node->var_decl.is_const ? "CONST" : "LET",
           SV_ARG(node->var_decl.name->var.value),
           type_to_name(node->var_decl.declared_type));
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
  case NODE_BOOL:
    printf("BOOL: %s\n", node->boolean.value ? "true" : "false");
    break;
  case NODE_STRING:
    printf("STRING: " SV_FMT "\n", SV_ARG(node->string.value));
    break;
  case NODE_VAR:
    printf("VAR: " SV_FMT "\n", SV_ARG(node->var.value));
    break;
  case NODE_BINARY_ARITH:
    printf("BINARY ARITH OP: %d\n", node->binary_arith.op);
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary_arith.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary_arith.right, indent + 2);
    break;
  case NODE_BINARY_COMPARE:
    printf("BINARY COMPARE OP: %d\n", node->binary_compare.op);
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary_compare.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary_compare.right, indent + 2);
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
  case NODE_BLOCK:
    printf("BLOCK\n");
    for (size_t i = 0; i < node->block.statements.len; i++) {
      AstNode *child = node->block.statements.items[i];
      Ast_print(child, indent + 1);
    }
    break;
  case NODE_EXPR_STMT:
    printf("EXPR_STMT\n");
    Ast_print(node->expr_stmt.expr, indent + 1);
    break;
  case NODE_TERNARY:
    printf("TERNARY\n");
    print_indent(indent + 1);
    printf("CONDITION:\n");
    Ast_print(node->ternary.condition, indent + 2);
    print_indent(indent + 1);
    printf("TRUE:\n");
    Ast_print(node->ternary.true_expr, indent + 2);
    print_indent(indent + 1);
    printf("FALSE:\n");
    Ast_print(node->ternary.false_expr, indent + 2);
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
  case NODE_IF_STMT:
    printf("IF_STMT\n");
    print_indent(indent + 1);
    printf("CONDITION:\n");
    Ast_print(node->if_stmt.condition, indent + 2);
    print_indent(indent + 1);
    printf("THEN:\n");
    Ast_print(node->if_stmt.then_branch, indent + 2);
    if (node->if_stmt.else_branch) {
      print_indent(indent + 1);
      printf("ELSE:\n");
      Ast_print(node->if_stmt.else_branch, indent + 2);
    }
    break;
  case NODE_DEFER:
    printf("DEFER\n");
    print_indent(indent + 1);
    printf("EXPR:\n");
    Ast_print(node->defer.expr, indent + 2);
    break;
  case NODE_ARRAY_LITERAL:
    printf("ARRAY_LITERAL\n");
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      Ast_print(node->array_literal.items.items[i], indent + 1);
    }
    break;
  case NODE_INDEX:
    printf("INDEX_EXPR\n");
    print_indent(indent + 1);
    printf("ARRAY:\n");
    Ast_print(node->index.array, indent + 2);
    print_indent(indent + 1);
    printf("INDEX:\n");
    Ast_print(node->index.index, indent + 2);
    break;
  case NODE_FIELD:
    printf("FIELD_ACCESS: " SV_FMT "\n", SV_ARG(node->field.field));
    print_indent(indent + 1);
    printf("OBJECT:\n");
    Ast_print(node->field.object, indent + 2);
    break;
  case NODE_ARRAY_REPEAT:
    printf("ARRAY_REPEAT\n");
    print_indent(indent + 1);
    printf("VALUE:\n");
    Ast_print(node->array_repeat.value, indent + 2);
    print_indent(indent + 1);
    printf("COUNT:\n");
    Ast_print(node->array_repeat.count, indent + 2);
    break;
  case NODE_INTRINSIC_COMPARE:
    printf("INTRINSIC_COMPARE (%s)\n",
           node->intrinsic_compare.op == OP_EQ ? "==" : "!=");
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->intrinsic_compare.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->intrinsic_compare.right, indent + 2);
    break;
  case NODE_STATIC_MEMBER:
    printf("STATIC_MEMBER: " SV_FMT "::" SV_FMT "\n",
           SV_ARG(node->static_member.parent),
           SV_ARG(node->static_member.member));
    break;
  default:
    printf("UNKNOWN NODE %d\n", node->tag);
    break;
  }
}
