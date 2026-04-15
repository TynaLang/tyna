#include "tyl/ast.h"
#include "tyl/sema.h"
#include <stdio.h>
#include <stdlib.h>

static AstNode *AstNode_new(AstKind ast_kind, Location loc) {
  AstNode *node = xmalloc(sizeof(AstNode));
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
                              int is_const, bool is_export, Location loc) {
  AstNode *node = AstNode_new(NODE_VAR_DECL, loc);
  node->var_decl.name = name;
  node->var_decl.value = value;
  node->var_decl.declared_type = type;
  node->var_decl.is_const = is_const;
  node->var_decl.is_export = is_export;
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

AstNode *AstNode_new_func_decl(AstNode *name, List params, Type *ret_type,
                               AstNode *body, bool is_static, bool is_export,
                               bool is_external, Location loc) {
  AstNode *node = AstNode_new(NODE_FUNC_DECL, loc);
  node->func_decl.name = name;
  node->func_decl.params = params;
  node->func_decl.return_type = ret_type;
  node->func_decl.body = body;
  node->func_decl.is_static = is_static;
  node->func_decl.is_export = is_export;
  node->func_decl.is_external = is_external;
  return node;
}

AstNode *AstNode_new_return(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_RETURN_STMT, loc);
  node->return_stmt.expr = expr;
  return node;
}

AstNode *AstNode_new_param(AstNode *name, Type *type, Location loc) {
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

AstNode *AstNode_new_switch_stmt(AstNode *expr, List cases, Location loc) {
  AstNode *node = AstNode_new(NODE_SWITCH_STMT, loc);
  node->switch_stmt.expr = expr;
  node->switch_stmt.cases = cases;
  return node;
}

AstNode *AstNode_new_case_stmt(AstNode *pattern, Type *pattern_type,
                               AstNode *body, Location loc) {
  AstNode *node = AstNode_new(NODE_CASE, loc);
  node->case_stmt.pattern = pattern;
  node->case_stmt.pattern_type = pattern_type;
  node->case_stmt.body = body;
  return node;
}

AstNode *AstNode_new_import(StringView path, StringView alias, Location loc) {
  AstNode *node = AstNode_new(NODE_IMPORT, loc);
  node->import.path = path;
  node->import.alias = alias;
  return node;
}

AstNode *AstNode_new_defer(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_DEFER, loc);
  node->defer.expr = expr;
  return node;
}

AstNode *AstNode_new_loop(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_LOOP_STMT, loc);
  node->loop.expr = expr;
  return node;
}

AstNode *AstNode_new_while(AstNode *condition, AstNode *body, Location loc) {
  AstNode *node = AstNode_new(NODE_WHILE_STMT, loc);
  node->while_stmt.condition = condition;
  node->while_stmt.body = body;
  return node;
}

AstNode *AstNode_new_for(AstNode *init, AstNode *cond, AstNode *inc,
                         AstNode *body, Location loc) {
  AstNode *node = AstNode_new(NODE_FOR_STMT, loc);
  node->for_stmt.init = init;
  node->for_stmt.condition = cond;
  node->for_stmt.increment = inc;
  node->for_stmt.body = body;
  return node;
}

AstNode *AstNode_new_for_in(AstNode *var, AstNode *iterable, AstNode *body,
                            Location loc) {
  AstNode *node = AstNode_new(NODE_FOR_IN_STMT, loc);
  node->for_in_stmt.var = var;
  node->for_in_stmt.iterable = iterable;
  node->for_in_stmt.body = body;
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

AstNode *AstNode_new_struct_decl(AstNode *name, List members, List placeholders,
                                 bool is_frozen, bool is_export, Location loc) {
  AstNode *node = AstNode_new(NODE_STRUCT_DECL, loc);
  node->struct_decl.name = name;
  node->struct_decl.members = members;
  node->struct_decl.placeholders = placeholders;
  node->struct_decl.is_frozen = is_frozen;
  node->struct_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_union_decl(AstNode *name, List members, List placeholders,
                                bool is_frozen, bool is_export, Location loc) {
  AstNode *node = AstNode_new(NODE_UNION_DECL, loc);
  node->union_decl.name = name;
  node->union_decl.members = members;
  node->union_decl.placeholders = placeholders;
  node->union_decl.is_frozen = is_frozen;
  node->union_decl.is_export = is_export;
  return node;
}

AstNode *AstNode_new_impl_decl(Type *type, List members, Location loc) {
  AstNode *node = AstNode_new(NODE_IMPL_DECL, loc);
  node->impl_decl.type = type;
  node->impl_decl.members = members;
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

AstNode *AstNode_new_break(Location loc) {
  AstNode *node = AstNode_new(NODE_BREAK, loc);
  return node;
}

AstNode *AstNode_new_continue(Location loc) {
  AstNode *node = AstNode_new(NODE_CONTINUE, loc);
  return node;
}

bool type_is_lvalue(AstNode *node) {
  if (node->tag == NODE_VAR || node->tag == NODE_FIELD ||
      node->tag == NODE_INDEX) {
    return true;
  }
  if (node->tag == NODE_UNARY && node->unary.op == OP_DEREF) {
    return true;
  }
  return false;
}

void Ast_free(AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {
  case NODE_BREAK:
  case NODE_CONTINUE:
    // These node kinds have no structs
    break;
  case NODE_NUMBER:
  case NODE_CHAR:
  case NODE_BOOL:
  case NODE_STRING:
  case NODE_VAR:
  case NODE_STATIC_MEMBER:
  case NODE_PARAM:
    // These nodes have no pointers
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
  case NODE_IMPL_DECL:
    for (size_t i = 0; i < node->impl_decl.members.len; i++) {
      Ast_free((AstNode *)node->impl_decl.members.items[i]);
    }
    List_free(&node->impl_decl.members, 0);
    break;
  case NODE_IMPORT:
    // import path and alias are views into source text, no heap ownership
    break;

    // end
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

  case NODE_VAR:
    copy->var = node->var;
    break;

  case NODE_BINARY_ARITH:
  case NODE_BINARY_COMPARE:
  case NODE_BINARY_EQUALITY:
  case NODE_BINARY_LOGICAL:
    copy->binary_arith.left = Ast_clone(node->binary_arith.left);
    copy->binary_arith.right = Ast_clone(node->binary_arith.right);
    copy->binary_arith.op = node->binary_arith.op;
    if (node->tag == NODE_BINARY_COMPARE)
      copy->binary_compare.op = node->binary_compare.op;
    if (node->tag == NODE_BINARY_EQUALITY)
      copy->binary_equality.op = node->binary_equality.op;
    if (node->tag == NODE_BINARY_LOGICAL)
      copy->binary_logical.op = node->binary_logical.op;
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
    break;

  case NODE_RETURN_STMT:
    copy->return_stmt.expr = Ast_clone(node->return_stmt.expr);
    break;

  case NODE_PARAM:
    copy->param.name = Ast_clone(node->param.name);
    copy->param.type = node->param.type;
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
