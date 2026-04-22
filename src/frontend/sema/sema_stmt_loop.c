#include "sema_internal.h"

void sema_check_loop_stmt(Sema *s, AstNode *node) {
  SemaJump jump_ctx;
  sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
  sema_check_stmt(s, node->loop.expr);
  sema_jump_pop(s);
}

void sema_check_while_stmt(Sema *s, AstNode *node) {
  Type *cond = sema_check_expr(s, node->while_stmt.condition).type;

  if (!type_is_condition(cond)) {
    sema_error(s, node->while_stmt.condition,
               "while condition must be bool or pointer, got '%s'",
               type_to_name(cond));
  }

  SemaJump jump_ctx;
  sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
  sema_check_stmt(s, node->while_stmt.body);
  sema_jump_pop(s);
}

void sema_check_for_stmt(Sema *s, AstNode *node) {
  sema_scope_push(s);

  if (node->for_stmt.init) {
    sema_check_stmt(s, node->for_stmt.init);
  }

  if (node->for_stmt.condition) {
    Type *cond = sema_check_expr(s, node->for_stmt.condition).type;
    if (!type_is_bool(cond)) {
      sema_error(s, node->for_stmt.condition,
                 "for condition must be bool, got '%s'", type_to_name(cond));
    }
  }

  if (node->for_stmt.increment) {
    sema_check_expr(s, node->for_stmt.increment);
  }

  SemaJump jump_ctx;
  sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, true, true);
  sema_check_stmt(s, node->for_stmt.body);
  sema_jump_pop(s);

  sema_scope_pop(s);
}
