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

void sema_check_if_stmt(Sema *s, AstNode *node) {
  Type *cond = sema_check_expr(s, node->if_stmt.condition).type;

  if (!type_is_condition(cond)) {
    sema_error(s, node->if_stmt.condition,
               "if condition must be bool or pointer, got '%s'",
               type_to_name(cond));
  }

  Type *narrow_type = get_if_is_narrow_type(s, node->if_stmt.condition);
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

  if (narrow_type) {
    AstNode *left = node->if_stmt.condition->binary_is.left;
    sema_scope_push(s);
    sema_define(s, left->var.value, narrow_type, false, left->loc);
    sema_check_stmt(s, node->if_stmt.then_branch);
    sema_scope_pop(s);
  } else {
    sema_check_stmt(s, node->if_stmt.then_branch);
  }

  if (node->if_stmt.else_branch) {
    sema_check_stmt(s, node->if_stmt.else_branch);
  }
}
