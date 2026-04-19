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

void sema_check_switch_stmt(Sema *s, AstNode *node) {
  Type *expr_type = sema_check_expr(s, node->switch_stmt.expr).type;
  if (!expr_type)
    return;

  SemaJump jump_ctx;
  sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, false, true);

  if (expr_type->kind == KIND_PRIMITIVE &&
      expr_type->data.primitive == PRIM_STRING) {
    for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
      AstNode *case_node = node->switch_stmt.cases.items[i];
      if (!case_node || case_node->tag != NODE_CASE) {
        continue;
      }
      if (case_node->case_stmt.pattern &&
          case_node->case_stmt.pattern->tag != NODE_STRING) {
        sema_error(s, case_node,
                   "String switch cases must use string literals or '_'");
        continue;
      }
      sema_check_stmt(s, case_node->case_stmt.body);
    }
  } else if (expr_type->kind == KIND_UNION && expr_type->is_tagged_union) {
    for (size_t i = 0; i < node->switch_stmt.cases.len; i++) {
      AstNode *case_node = node->switch_stmt.cases.items[i];
      if (!case_node || case_node->tag != NODE_CASE) {
        continue;
      }
      if (!case_node->case_stmt.pattern) {
        sema_check_stmt(s, case_node->case_stmt.body);
        continue;
      }
      if (case_node->case_stmt.pattern->tag != NODE_VAR ||
          !case_node->case_stmt.pattern_type) {
        sema_error(s, case_node,
                   "Union switch cases must use 'case name: Type => ...'");
        continue;
      }

      Type *variant_type = case_node->case_stmt.pattern_type;
      bool found = false;
      for (size_t j = 0; j < expr_type->members.len; j++) {
        Member *m = expr_type->members.items[j];
        if (type_equals(m->type, variant_type)) {
          found = true;
          break;
        }
      }
      if (!found) {
        sema_error(s, case_node, "Type '%s' is not a variant of union '%s'",
                   type_to_name(variant_type), type_to_name(expr_type));
        continue;
      }

      sema_scope_push(s);
      sema_define(s, case_node->case_stmt.pattern->var.value, variant_type,
                  false, case_node->loc);
      sema_check_stmt(s, case_node->case_stmt.body);
      sema_scope_pop(s);
    }
  } else {
    sema_error(s, node,
               "Switch expression must be a string or tagged union, got '%s'",
               type_to_name(expr_type));
  }

  sema_jump_pop(s);
}
