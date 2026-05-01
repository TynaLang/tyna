#include "sema_internal.h"

static Type *get_if_is_narrow_type(Sema *s, AstNode *cond) {
  if (!cond || cond->tag != NODE_BINARY_IS)
    return NULL;

  AstNode *left = cond->binary_is.left;
  AstNode *right = cond->binary_is.right;
  if (!left || left->tag != NODE_VAR)
    return NULL;

  Type *left_type = sema_check_expr(s, left).type;
  Type *right_type = sema_check_expr(s, right).type;

  if (left_type->kind == KIND_RESULT && right_type->kind == KIND_ERROR) {
    return right_type;
  }
  return NULL;
}

static bool get_if_is_pattern_binder(Sema *s, AstNode *cond,
                                     StringView *out_name, Type **out_type) {
  if (!cond || cond->tag != NODE_BINARY_IS || !out_name || !out_type)
    return false;

  AstNode *right = cond->binary_is.right;
  if (!right || right->tag != NODE_CALL)
    return false;
  if (right->call.func->tag != NODE_STATIC_MEMBER)
    return false;

  Type *parent_type =
      sema_find_type_by_name(s, right->call.func->static_member.parent);
  if (!parent_type || parent_type->kind != KIND_UNION ||
      !parent_type->is_tagged_union)
    return false;

  Member *variant =
      type_get_member(parent_type, right->call.func->static_member.member);
  if (!variant)
    return false;

  if (right->call.args.len != 1)
    return false;

  AstNode *arg_node = right->call.args.items[0];
  if (!arg_node || arg_node->tag != NODE_VAR)
    return false;

  Symbol *symbol = sema_resolve(s, arg_node->var.value);
  if (symbol)
    return false;

  *out_name = arg_node->var.value;
  *out_type = variant->type;
  return true;
}

void sema_check_if_stmt(Sema *s, AstNode *node) {
  Type *cond = sema_check_expr(s, node->if_stmt.condition).type;

  if (!type_is_condition(cond)) {
    sema_error(s, node->if_stmt.condition,
               "if condition must be bool or pointer, got '%s'",
               type_to_name(cond));
  }

  Type *narrow_type = get_if_is_narrow_type(s, node->if_stmt.condition);
  StringView binder_name = sv_from_parts("", 0);
  Type *binder_type = NULL;
  bool has_binder = get_if_is_pattern_binder(s, node->if_stmt.condition,
                                             &binder_name, &binder_type);
  if (node->if_stmt.then_branch &&
      node->if_stmt.then_branch->tag != NODE_BLOCK) {
    AstNode *block = AstNode_new_block(node->if_stmt.then_branch->loc);
    List_init(&block->block.statements);
    List_push(&block->block.statements, node->if_stmt.then_branch);
    node->if_stmt.then_branch = block;
  }
  if (node->if_stmt.else_branch &&
      node->if_stmt.else_branch->tag != NODE_BLOCK) {
    AstNode *block = AstNode_new_block(node->if_stmt.else_branch->loc);
    List_init(&block->block.statements);
    List_push(&block->block.statements, node->if_stmt.else_branch);
    node->if_stmt.else_branch = block;
  }

  if (narrow_type || has_binder) {
    sema_scope_push(s);
    if (narrow_type) {
      AstNode *left = node->if_stmt.condition->binary_is.left;
      sema_define(s, left->var.value, narrow_type, false, left->loc);
    }
    if (has_binder) {
      sema_define(s, binder_name, binder_type, false,
                  node->if_stmt.condition->binary_is.right->loc);
    }
    sema_check_stmt(s, node->if_stmt.then_branch);
    sema_scope_pop(s);
  } else {
    sema_check_stmt(s, node->if_stmt.then_branch);
  }

  if (node->if_stmt.else_branch) {
    sema_check_stmt(s, node->if_stmt.else_branch);
  }
}

ExprInfo sema_check_if_expr(Sema *s, AstNode *node) {
  Type *cond = sema_check_expr(s, node->if_stmt.condition).type;
  if (!type_is_condition(cond)) {
    sema_error(s, node->if_stmt.condition,
               "if condition must be bool or pointer, got '%s'",
               type_to_name(cond));
  }

  Type *narrow_type = get_if_is_narrow_type(s, node->if_stmt.condition);
  StringView binder_name = sv_from_parts("", 0);
  Type *binder_type = NULL;
  bool has_binder = get_if_is_pattern_binder(s, node->if_stmt.condition,
                                             &binder_name, &binder_type);
  ExprInfo then_info;
  ExprInfo else_info;

  if (narrow_type || has_binder) {
    sema_scope_push(s);
    if (narrow_type) {
      AstNode *left = node->if_stmt.condition->binary_is.left;
      sema_define(s, left->var.value, narrow_type, false, left->loc);
    }
    if (has_binder) {
      sema_define(s, binder_name, binder_type, false,
                  node->if_stmt.condition->binary_is.right->loc);
    }
    then_info = sema_check_expr(s, node->if_stmt.then_branch);
    sema_scope_pop(s);
  } else {
    then_info = sema_check_expr(s, node->if_stmt.then_branch);
  }

  if (node->if_stmt.else_branch) {
    else_info = sema_check_expr(s, node->if_stmt.else_branch);
  } else {
    else_info = (ExprInfo){.type = type_get_primitive(s->types, PRIM_VOID),
                           .category = VAL_RVALUE};
  }

  if (then_info.type->kind == KIND_PRIMITIVE &&
      then_info.type->data.primitive == PRIM_UNKNOWN) {
    node->if_stmt.then_branch->resolved_type = else_info.type;
    return else_info;
  }
  if (else_info.type->kind == KIND_PRIMITIVE &&
      else_info.type->data.primitive == PRIM_UNKNOWN) {
    node->if_stmt.else_branch->resolved_type = then_info.type;
    return then_info;
  }

  if (type_can_implicitly_cast(then_info.type, else_info.type)) {
    sema_check_literal_bounds(s, node->if_stmt.then_branch, else_info.type);
    sema_check_literal_bounds(s, node->if_stmt.else_branch, else_info.type);
    return (ExprInfo){.type = else_info.type, .category = VAL_RVALUE};
  }
  if (type_can_implicitly_cast(else_info.type, then_info.type)) {
    sema_check_literal_bounds(s, node->if_stmt.then_branch, then_info.type);
    sema_check_literal_bounds(s, node->if_stmt.else_branch, then_info.type);
    return (ExprInfo){.type = then_info.type, .category = VAL_RVALUE};
  }

  if (node->if_stmt.else_branch) {
    sema_error(
        s, node,
        "If-expression branches must have compatible types: got %s and %s",
        type_to_name(then_info.type), type_to_name(else_info.type));
  }
  return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                    .category = VAL_RVALUE};
}
