#include "sema_internal.h"

void sema_check_stmt(Sema *s, AstNode *node) {
  if (!node)
    return;

  switch (node->tag) {

  case NODE_IMPORT: {
    sema_check_import(s, node);
    break;
  }

  case NODE_VAR_DECL: {
    sema_check_var_decl(s, node);
    break;
  }

  case NODE_FUNC_DECL: {
    if (s->pass == SEMA_PASS_BODIES) {
      sema_check_func_decl(s, node);
    }
    break;
  }

  case NODE_BLOCK: {
    sema_check_block(s, node);
    break;
  }

  case NODE_RETURN_STMT: {
    sema_check_return_stmt(s, node);
    break;
  }

  case NODE_IF_STMT: {
    sema_check_if_stmt(s, node);
    break;
  }

  case NODE_SWITCH_STMT: {
    sema_check_switch_stmt(s, node);
    break;
  }
  case NODE_LOOP_STMT: {
    sema_check_loop_stmt(s, node);
    break;
  }

  case NODE_WHILE_STMT: {
    sema_check_while_stmt(s, node);
    break;
  }

  case NODE_FOR_STMT: {
    sema_check_for_stmt(s, node);
    break;
  }

  case NODE_FOR_IN_STMT: {
    sema_check_for_in_stmt(s, node);
    break;
  }

  case NODE_STRUCT_DECL: {
    sema_check_struct_decl(s, node);
    break;
  }
  case NODE_UNION_DECL: {
    sema_check_union_decl(s, node);
    break;
  }
  case NODE_ERROR_DECL: {
    sema_check_error_decl(s, node);
    break;
  }
  case NODE_ERROR_SET_DECL: {
    sema_check_error_set_decl(s, node);
    break;
  }
  case NODE_IMPL_DECL: {
    sema_check_impl_decl(s, node);
    break;
  }

  case NODE_TYPE_ALIAS: {
    node->resolved_type = node->type_alias_decl.target_type;
    break;
  }

  case NODE_PRINT_STMT: {
    for (size_t i = 0; i < node->print_stmt.values.len; i++) {
      sema_check_expr(s, node->print_stmt.values.items[i]);
    }
    break;
  }

  case NODE_EXPR_STMT: {
    sema_check_expr(s, node->expr_stmt.expr);
    break;
  }

  case NODE_DEFER: {
    sema_check_stmt(s, node->defer.expr);
    break;
  }

  case NODE_BREAK: {
    SemaJump *jump = sema_find_break_jump(s);
    if (!jump) {
      sema_error(s, node, "break statement outside of loop or switch");
    }
    break;
  }

  case NODE_CONTINUE: {
    SemaJump *loop = sema_find_loop_jump(s);
    if (!loop) {
      sema_error(s, node, "continue statement outside of loop");
    }
    break;
  }

  default:
    break;
  }
}