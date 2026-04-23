#include "tyna/ast.h"
#include <stdio.h>
#include <stdlib.h>

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

static FILE *ast_stream = NULL;

static void print_indent(int indent) {
  FILE *out = ast_stream ? ast_stream : stdout;
  for (int i = 0; i < indent; i++) {
    fprintf(out, "  ");
  }
}

void Ast_free(AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_BREAK:
  case NODE_CONTINUE:
    break;
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_BOOL:
  case NODE_STRING:
  case NODE_VAR:
  case NODE_STATIC_MEMBER:
    break;
  case NODE_PARAM:
    Ast_free(node->param.name);
    Ast_free(node->param.default_value);
    break;
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
  case NODE_NULL:
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
  case NODE_BINARY_IS:
    Ast_free(node->binary_is.left);
    Ast_free(node->binary_is.right);
    break;
  case NODE_BINARY_ELSE:
    Ast_free(node->binary_else.left);
    Ast_free(node->binary_else.right);
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
    List_free(&node->call.generic_args, 0);
    break;
  case NODE_NEW_EXPR:
    for (size_t i = 0; i < node->new_expr.args.len; i++) {
      Ast_free((AstNode *)node->new_expr.args.items[i]);
    }
    List_free(&node->new_expr.args, 0);
    for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
      Ast_free((AstNode *)node->new_expr.field_inits.items[i]);
    }
    List_free(&node->new_expr.field_inits, 0);
    break;
  case NODE_FUNC_DECL:
    for (size_t i = 0; i < node->func_decl.params.len; i++) {
      Ast_free((AstNode *)node->func_decl.params.items[i]);
    }
    List_free(&node->func_decl.params, 0);
    Ast_free(node->func_decl.body);
    break;
  case NODE_FIELD:
    Ast_free(node->field.object);
    break;
  case NODE_RETURN_STMT:
    Ast_free(node->return_stmt.expr);
    break;
  case NODE_BLOCK:
    for (size_t i = 0; i < node->block.statements.len; i++) {
      Ast_free((AstNode *)node->block.statements.items[i]);
    }
    List_free(&node->block.statements, 0);
    break;
  case NODE_INDEX:
    Ast_free(node->index.index);
    Ast_free(node->index.array);
    break;
  case NODE_IF_STMT:
    Ast_free(node->if_stmt.condition);
    Ast_free(node->if_stmt.then_branch);
    Ast_free(node->if_stmt.else_branch);
    break;
  case NODE_SWITCH_STMT:
    Ast_free(node->switch_stmt.expr);
    for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
      Ast_free((AstNode *)node->switch_stmt.cases.items[i]);
    }
    List_free(&node->switch_stmt.cases, 0);
    break;
  case NODE_CASE:
    Ast_free(node->case_stmt.pattern);
    Ast_free(node->case_stmt.body);
    break;
  case NODE_DEFER:
    Ast_free(node->defer.expr);
    break;
  case NODE_LOOP_STMT:
    Ast_free(node->loop.expr);
    break;
  case NODE_WHILE_STMT:
    Ast_free(node->while_stmt.condition);
    Ast_free(node->while_stmt.body);
    break;
  case NODE_FOR_STMT:
    Ast_free(node->for_stmt.init);
    Ast_free(node->for_stmt.condition);
    Ast_free(node->for_stmt.increment);
    Ast_free(node->for_stmt.body);
    break;
  case NODE_FOR_IN_STMT:
    Ast_free(node->for_in_stmt.var);
    Ast_free(node->for_in_stmt.iterable);
    Ast_free(node->for_in_stmt.body);
    break;
  case NODE_ARRAY_LITERAL:
    for (size_t i = 0; i < node->array_literal.items.len; i++) {
      Ast_free((AstNode *)node->array_literal.items.items[i]);
    }
    List_free(&node->array_literal.items, 0);
    break;
  case NODE_ARRAY_REPEAT:
    Ast_free(node->array_repeat.value);
    Ast_free(node->array_repeat.count);
    break;
  case NODE_INTRINSIC_COMPARE:
    Ast_free(node->intrinsic_compare.left);
    Ast_free(node->intrinsic_compare.right);
    break;
  case NODE_STRUCT_DECL:
    Ast_free(node->struct_decl.name);
    for (size_t i = 0; i < node->struct_decl.members.len; i++) {
      Ast_free((AstNode *)node->struct_decl.members.items[i]);
    }
    List_free(&node->struct_decl.members, 0);
    break;
  case NODE_UNION_DECL:
    Ast_free(node->union_decl.name);
    for (size_t i = 0; i < node->union_decl.members.len; i++) {
      Ast_free((AstNode *)node->union_decl.members.items[i]);
    }
    List_free(&node->union_decl.members, 0);
    break;
  case NODE_ERROR_DECL:
    Ast_free(node->error_decl.name);
    for (size_t i = 0; i < node->error_decl.members.len; i++) {
      Ast_free((AstNode *)node->error_decl.members.items[i]);
    }
    List_free(&node->error_decl.members, 0);
    break;
  case NODE_ERROR_SET_DECL:
    Ast_free(node->error_set_decl.name);
    for (size_t i = 0; i < node->error_set_decl.members.len; i++) {
      Ast_free((AstNode *)node->error_set_decl.members.items[i]);
    }
    List_free(&node->error_set_decl.members, 0);
    break;
  case NODE_IMPL_DECL:
    for (size_t i = 0; i < node->impl_decl.members.len; i++) {
      Ast_free((AstNode *)node->impl_decl.members.items[i]);
    }
    List_free(&node->impl_decl.members, 0);
    break;
  case NODE_IMPORT:
    break;

  default:
    break;
  }

  free(node);
}

AstNode *Ast_clone(AstNode *node) {
  if (!node)
    return NULL;

  AstNode *copy = xcalloc(1, sizeof(AstNode));
  copy->tag = node->tag;
  copy->loc = node->loc;
  copy->resolved_type = node->resolved_type;

  switch (node->tag) {
  case NODE_AST_ROOT:
    List_init(&copy->ast_root.children);
    for (size_t i = 0; i < node->ast_root.children.len; i++)
      List_push(&copy->ast_root.children,
                Ast_clone(node->ast_root.children.items[i]));
    break;

  case NODE_VAR_DECL:
    copy->var_decl.name = Ast_clone(node->var_decl.name);
    copy->var_decl.value = Ast_clone(node->var_decl.value);
    copy->var_decl.declared_type = node->var_decl.declared_type;
    copy->var_decl.is_const = node->var_decl.is_const;
    copy->var_decl.is_export = node->var_decl.is_export;
    break;

  case NODE_PRINT_STMT:
    List_init(&copy->print_stmt.values);
    for (size_t i = 0; i < node->print_stmt.values.len; i++)
      List_push(&copy->print_stmt.values,
                Ast_clone(node->print_stmt.values.items[i]));
    break;

  case NODE_NUMBER:
    copy->number = node->number;
    break;

  case NODE_CHAR:
    copy->char_lit = node->char_lit;
    break;

  case NODE_BOOL:
    copy->boolean = node->boolean;
    break;

  case NODE_STRING:
    copy->string = node->string;
    break;

  case NODE_NULL:
    break;

  case NODE_VAR:
    copy->var = node->var;
    break;

  case NODE_BINARY_ARITH:
    copy->binary_arith.left = Ast_clone(node->binary_arith.left);
    copy->binary_arith.right = Ast_clone(node->binary_arith.right);
    copy->binary_arith.op = node->binary_arith.op;
    break;
  case NODE_BINARY_COMPARE:
    copy->binary_compare.left = Ast_clone(node->binary_compare.left);
    copy->binary_compare.right = Ast_clone(node->binary_compare.right);
    copy->binary_compare.op = node->binary_compare.op;
    break;
  case NODE_BINARY_EQUALITY:
    copy->binary_equality.left = Ast_clone(node->binary_equality.left);
    copy->binary_equality.right = Ast_clone(node->binary_equality.right);
    copy->binary_equality.op = node->binary_equality.op;
    break;
  case NODE_BINARY_LOGICAL:
    copy->binary_logical.left = Ast_clone(node->binary_logical.left);
    copy->binary_logical.right = Ast_clone(node->binary_logical.right);
    copy->binary_logical.op = node->binary_logical.op;
    break;
  case NODE_BINARY_IS:
    copy->binary_is.left = Ast_clone(node->binary_is.left);
    copy->binary_is.right = Ast_clone(node->binary_is.right);
    break;
  case NODE_BINARY_ELSE:
    copy->binary_else.left = Ast_clone(node->binary_else.left);
    copy->binary_else.right = Ast_clone(node->binary_else.right);
    break;

  case NODE_UNARY:
    copy->unary.op = node->unary.op;
    copy->unary.expr = Ast_clone(node->unary.expr);
    break;

  case NODE_CAST_EXPR:
    copy->cast_expr.expr = Ast_clone(node->cast_expr.expr);
    copy->cast_expr.target_type = node->cast_expr.target_type;
    break;

  case NODE_ASSIGN_EXPR:
    copy->assign_expr.target = Ast_clone(node->assign_expr.target);
    copy->assign_expr.value = Ast_clone(node->assign_expr.value);
    break;

  case NODE_EXPR_STMT:
    copy->expr_stmt.expr = Ast_clone(node->expr_stmt.expr);
    break;

  case NODE_TERNARY:
    copy->ternary.condition = Ast_clone(node->ternary.condition);
    copy->ternary.true_expr = Ast_clone(node->ternary.true_expr);
    copy->ternary.false_expr = Ast_clone(node->ternary.false_expr);
    break;

  case NODE_CALL:
    copy->call.func = Ast_clone(node->call.func);
    List_init(&copy->call.args);
    for (size_t i = 0; i < node->call.args.len; i++)
      List_push(&copy->call.args, Ast_clone(node->call.args.items[i]));
    List_init(&copy->call.generic_args);
    for (size_t i = 0; i < node->call.generic_args.len; i++)
      List_push(&copy->call.generic_args, node->call.generic_args.items[i]);
    break;

  case NODE_NEW_EXPR:
    copy->new_expr.target_type = node->new_expr.target_type;
    List_init(&copy->new_expr.args);
    for (size_t i = 0; i < node->new_expr.args.len; i++)
      List_push(&copy->new_expr.args, Ast_clone(node->new_expr.args.items[i]));
    List_init(&copy->new_expr.field_inits);
    for (size_t i = 0; i < node->new_expr.field_inits.len; i++)
      List_push(&copy->new_expr.field_inits,
                Ast_clone(node->new_expr.field_inits.items[i]));
    break;

  case NODE_RETURN_STMT:
    copy->return_stmt.expr = Ast_clone(node->return_stmt.expr);
    break;

  case NODE_PARAM:
    copy->param.name = Ast_clone(node->param.name);
    copy->param.type = node->param.type;
    copy->param.default_value = Ast_clone(node->param.default_value);
    copy->param.requires_storage = node->param.requires_storage;
    break;

  case NODE_BLOCK:
    List_init(&copy->block.statements);
    for (size_t i = 0; i < node->block.statements.len; i++)
      List_push(&copy->block.statements,
                Ast_clone(node->block.statements.items[i]));
    break;

  case NODE_FIELD:
    copy->field.object = Ast_clone(node->field.object);
    copy->field.field = node->field.field;
    break;

  case NODE_STATIC_MEMBER:
    copy->static_member.parent = node->static_member.parent;
    copy->static_member.member = node->static_member.member;
    break;

  case NODE_INDEX:
    copy->index.array = Ast_clone(node->index.array);
    copy->index.index = Ast_clone(node->index.index);
    break;

  case NODE_FUNC_DECL:
    copy->func_decl.name = Ast_clone(node->func_decl.name);
    List_init(&copy->func_decl.params);
    for (size_t i = 0; i < node->func_decl.params.len; i++)
      List_push(&copy->func_decl.params,
                Ast_clone(node->func_decl.params.items[i]));
    copy->func_decl.body = Ast_clone(node->func_decl.body);
    copy->func_decl.return_type = node->func_decl.return_type;
    copy->func_decl.is_static = node->func_decl.is_static;
    copy->func_decl.is_export = node->func_decl.is_export;
    copy->func_decl.is_external = node->func_decl.is_external;
    copy->func_decl.requires_arena = node->func_decl.requires_arena;
    copy->func_decl.consumes_string_arg = node->func_decl.consumes_string_arg;
    break;

  case NODE_STRUCT_DECL:
    copy->struct_decl.name = Ast_clone(node->struct_decl.name);
    List_init(&copy->struct_decl.members);
    for (size_t i = 0; i < node->struct_decl.members.len; i++)
      List_push(&copy->struct_decl.members,
                Ast_clone(node->struct_decl.members.items[i]));
    List_init(&copy->struct_decl.placeholders);
    for (size_t i = 0; i < node->struct_decl.placeholders.len; i++)
      List_push(&copy->struct_decl.placeholders,
                node->struct_decl.placeholders.items[i]);
    copy->struct_decl.is_frozen = node->struct_decl.is_frozen;
    copy->struct_decl.is_export = node->struct_decl.is_export;
    break;

  case NODE_UNION_DECL:
    copy->union_decl.name = Ast_clone(node->union_decl.name);
    List_init(&copy->union_decl.members);
    for (size_t i = 0; i < node->union_decl.members.len; i++)
      List_push(&copy->union_decl.members,
                Ast_clone(node->union_decl.members.items[i]));
    List_init(&copy->union_decl.placeholders);
    for (size_t i = 0; i < node->union_decl.placeholders.len; i++)
      List_push(&copy->union_decl.placeholders,
                node->union_decl.placeholders.items[i]);
    copy->union_decl.is_frozen = node->union_decl.is_frozen;
    copy->union_decl.is_export = node->union_decl.is_export;
    break;

  case NODE_ERROR_DECL:
    copy->error_decl.name = Ast_clone(node->error_decl.name);
    List_init(&copy->error_decl.members);
    for (size_t i = 0; i < node->error_decl.members.len; i++)
      List_push(&copy->error_decl.members,
                Ast_clone(node->error_decl.members.items[i]));
    copy->error_decl.message = node->error_decl.message;
    copy->error_decl.is_export = node->error_decl.is_export;
    break;

  case NODE_ERROR_SET_DECL:
    copy->error_set_decl.name = Ast_clone(node->error_set_decl.name);
    List_init(&copy->error_set_decl.members);
    for (size_t i = 0; i < node->error_set_decl.members.len; i++)
      List_push(&copy->error_set_decl.members,
                Ast_clone(node->error_set_decl.members.items[i]));
    copy->error_set_decl.is_export = node->error_set_decl.is_export;
    break;

  case NODE_IMPL_DECL:
    copy->impl_decl.type = node->impl_decl.type;
    List_init(&copy->impl_decl.members);
    for (size_t i = 0; i < node->impl_decl.members.len; i++)
      List_push(&copy->impl_decl.members,
                Ast_clone(node->impl_decl.members.items[i]));
    break;

  case NODE_ARRAY_LITERAL:
    List_init(&copy->array_literal.items);
    for (size_t i = 0; i < node->array_literal.items.len; i++)
      List_push(&copy->array_literal.items,
                Ast_clone(node->array_literal.items.items[i]));
    break;

  case NODE_ARRAY_REPEAT:
    copy->array_repeat.value = Ast_clone(node->array_repeat.value);
    copy->array_repeat.count = Ast_clone(node->array_repeat.count);
    break;

  case NODE_IF_STMT:
    copy->if_stmt.condition = Ast_clone(node->if_stmt.condition);
    copy->if_stmt.then_branch = Ast_clone(node->if_stmt.then_branch);
    copy->if_stmt.else_branch = Ast_clone(node->if_stmt.else_branch);
    break;

  case NODE_SWITCH_STMT:
    copy->switch_stmt.expr = Ast_clone(node->switch_stmt.expr);
    List_init(&copy->switch_stmt.cases);
    for (size_t i = 0; i < node->switch_stmt.cases.len; i++)
      List_push(&copy->switch_stmt.cases,
                Ast_clone(node->switch_stmt.cases.items[i]));
    break;

  case NODE_CASE:
    copy->case_stmt.pattern = Ast_clone(node->case_stmt.pattern);
    copy->case_stmt.body = Ast_clone(node->case_stmt.body);
    break;

  case NODE_DEFER:
    copy->defer.expr = Ast_clone(node->defer.expr);
    break;

  case NODE_LOOP_STMT:
    copy->loop.expr = Ast_clone(node->loop.expr);
    break;

  case NODE_WHILE_STMT:
    copy->while_stmt.condition = Ast_clone(node->while_stmt.condition);
    copy->while_stmt.body = Ast_clone(node->while_stmt.body);
    break;

  case NODE_FOR_STMT:
    copy->for_stmt.init = Ast_clone(node->for_stmt.init);
    copy->for_stmt.condition = Ast_clone(node->for_stmt.condition);
    copy->for_stmt.increment = Ast_clone(node->for_stmt.increment);
    copy->for_stmt.body = Ast_clone(node->for_stmt.body);
    break;

  case NODE_FOR_IN_STMT:
    copy->for_in_stmt.var = Ast_clone(node->for_in_stmt.var);
    copy->for_in_stmt.iterable = Ast_clone(node->for_in_stmt.iterable);
    copy->for_in_stmt.body = Ast_clone(node->for_in_stmt.body);
    break;

  case NODE_INTRINSIC_COMPARE:
    copy->intrinsic_compare.left = Ast_clone(node->intrinsic_compare.left);
    copy->intrinsic_compare.right = Ast_clone(node->intrinsic_compare.right);
    copy->intrinsic_compare.op = node->intrinsic_compare.op;
    break;

  default:
    panic("Unsupported AST clone kind: %d", node->tag);
  }

  return copy;
}

void Ast_print_to_stream(FILE *out, AstNode *node, int indent) {
  if (!node)
    return;

  FILE *prev_stream = ast_stream;
  ast_stream = out ? out : stdout;
#define Ast_print(node, indent) Ast_print_to_stream(ast_stream, node, indent)
#define printf(...) fprintf(ast_stream, __VA_ARGS__)

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
    if (node->resolved_type &&
        node->resolved_type != node->var_decl.declared_type) {
      print_indent(indent + 1);
      printf("RESOLVED TYPE: %s\n", type_to_name(node->resolved_type));
    }
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
  case NODE_BINARY_EQUALITY:
    printf("BINARY EQUALITY OP: %d\n", node->binary_equality.op);
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary_equality.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary_equality.right, indent + 2);
    break;
  case NODE_BINARY_LOGICAL:
    printf("BINARY LOGICAL OP: %d\n", node->binary_logical.op);
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary_logical.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary_logical.right, indent + 2);
    break;
  case NODE_BINARY_IS:
    printf("BINARY IS\n");
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary_is.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary_is.right, indent + 2);
    break;
  case NODE_BINARY_ELSE:
    printf("BINARY ELSE\n");
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary_else.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary_else.right, indent + 2);
    break;
  case NODE_NULL:
    printf("NULL\n");
    break;
  case NODE_ERROR_DECL:
    printf("ERROR_DECL: " SV_FMT "\n",
           SV_ARG(node->error_decl.name->var.value));
    for (size_t i = 0; i < node->error_decl.members.len; i++) {
      Ast_print(node->error_decl.members.items[i], indent + 1);
    }
    break;
  case NODE_ERROR_SET_DECL:
    printf("ERROR_SET_DECL: " SV_FMT "\n",
           SV_ARG(node->error_set_decl.name->var.value));
    for (size_t i = 0; i < node->error_set_decl.members.len; i++) {
      Ast_print(node->error_set_decl.members.items[i], indent + 1);
    }
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
    printf("FUNC DECL: " SV_FMT " RETURNS %s\n",
           SV_ARG(node->func_decl.name->var.value),
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
    printf("PARAM: " SV_FMT " : %s\n", SV_ARG(node->param.name->var.value),
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
  case NODE_SWITCH_STMT:
    printf("SWITCH_STMT\n");
    print_indent(indent + 1);
    printf("EXPR:\n");
    Ast_print(node->switch_stmt.expr, indent + 2);
    for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
      print_indent(indent + 1);
      printf("CASE:\n");
      Ast_print((AstNode *)node->switch_stmt.cases.items[i], indent + 2);
    }
    break;
  case NODE_CASE:
    printf("CASE_ENTRY\n");
    if (node->case_stmt.pattern) {
      print_indent(indent + 1);
      printf("PATTERN:\n");
      Ast_print(node->case_stmt.pattern, indent + 2);
    }
    if (node->case_stmt.pattern_type) {
      print_indent(indent + 1);
      printf("TYPE: %s\n", type_to_name(node->case_stmt.pattern_type));
    }
    if (node->case_stmt.body) {
      print_indent(indent + 1);
      printf("BODY:\n");
      Ast_print(node->case_stmt.body, indent + 2);
    }
    break;
  case NODE_DEFER:
    printf("DEFER\n");
    print_indent(indent + 1);
    printf("EXPR:\n");
    Ast_print(node->defer.expr, indent + 2);
    break;
  case NODE_LOOP_STMT:
    printf("LOOP\n");
    print_indent(indent + 1);
    printf("EXPR:\n");
    Ast_print(node->loop.expr, indent + 2);
    break;
  case NODE_WHILE_STMT:
    printf("WHILE_STMT\n");
    print_indent(indent + 1);
    printf("CONDITION:\n");
    Ast_print(node->while_stmt.condition, indent + 2);
    print_indent(indent + 1);
    printf("BODY:\n");
    Ast_print(node->while_stmt.body, indent + 2);
    break;
  case NODE_FOR_STMT:
    printf("FOR_STMT\n");
    if (node->for_stmt.init) {
      print_indent(indent + 1);
      printf("INIT:\n");
      Ast_print(node->for_stmt.init, indent + 2);
    }
    if (node->for_stmt.condition) {
      print_indent(indent + 1);
      printf("CONDITION:\n");
      Ast_print(node->for_stmt.condition, indent + 2);
    }
    if (node->for_stmt.increment) {
      print_indent(indent + 1);
      printf("INCREMENT:\n");
      Ast_print(node->for_stmt.increment, indent + 2);
    }
    print_indent(indent + 1);
    printf("BODY:\n");
    Ast_print(node->for_stmt.body, indent + 2);
    break;
  case NODE_FOR_IN_STMT:
    printf("FOR_IN_STMT\n");
    print_indent(indent + 1);
    printf("VAR:\n");
    Ast_print(node->for_in_stmt.var, indent + 2);
    print_indent(indent + 1);
    printf("ITERABLE:\n");
    Ast_print(node->for_in_stmt.iterable, indent + 2);
    print_indent(indent + 1);
    printf("BODY:\n");
    Ast_print(node->for_in_stmt.body, indent + 2);
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
  case NODE_NEW_EXPR:
    printf("NEW_EXPR: %s\n", type_to_name(node->new_expr.target_type));
    if (node->new_expr.args.len > 0) {
      print_indent(indent + 1);
      printf("ARGS:\n");
      for (size_t i = 0; i < node->new_expr.args.len; i++) {
        Ast_print(node->new_expr.args.items[i], indent + 2);
      }
    }
    if (node->new_expr.field_inits.len > 0) {
      print_indent(indent + 1);
      printf("FIELD_INITS:\n");
      for (size_t i = 0; i < node->new_expr.field_inits.len; i++) {
        Ast_print(node->new_expr.field_inits.items[i], indent + 2);
      }
    }
    break;
  case NODE_FIELD:
    printf("FIELD_ACCESS: " SV_FMT "\n", SV_ARG(node->field.field));
    print_indent(indent + 1);
    printf("OBJECT:\n");
    Ast_print(node->field.object, indent + 2);
    break;
  case NODE_IMPORT:
    printf("IMPORT: path='" SV_FMT "' alias='" SV_FMT "'\n",
           SV_ARG(node->import.path), SV_ARG(node->import.alias));
    break;
  case NODE_ARRAY_REPEAT:
    printf("ARRAY_REPEAT (Type: %s)\n", type_to_name(node->resolved_type));
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
  case NODE_STRUCT_DECL:
    printf("STRUCT_DECL\n");
    print_indent(indent + 1);
    printf("NAME:\n");
    Ast_print(node->struct_decl.name, indent + 2);
    print_indent(indent + 1);
    printf("MEMBERS:\n");
    for (size_t i = 0; i < node->struct_decl.members.len; i++) {
      Ast_print(node->struct_decl.members.items[i], indent + 2);
    }
    break;
  case NODE_UNION_DECL:
    printf("UNION_DECL\n");
    print_indent(indent + 1);
    printf("NAME:\n");
    Ast_print(node->union_decl.name, indent + 2);
    print_indent(indent + 1);
    printf("MEMBERS:\n");
    for (size_t i = 0; i < node->union_decl.members.len; i++) {
      Ast_print(node->union_decl.members.items[i], indent + 2);
    }
    break;
  case NODE_IMPL_DECL:
    printf("IMPL_DECL\n");
    print_indent(indent + 1);
    printf("TYPE: %s\n", type_to_name(node->impl_decl.type));
    print_indent(indent + 1);
    printf("MEMBERS:\n");
    for (size_t i = 0; i < node->impl_decl.members.len; i++) {
      Ast_print(node->impl_decl.members.items[i], indent + 2);
    }
    break;
  case NODE_BREAK:
    printf("BREAK\n");
    break;
  case NODE_CONTINUE:
    printf("CONTINUE\n");
    break;
  }
#undef printf
#undef Ast_print
  ast_stream = prev_stream;
}

void Ast_print(AstNode *node, int indent) {
  Ast_print_to_stream(stdout, node, indent);
}
