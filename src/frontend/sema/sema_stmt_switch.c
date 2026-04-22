#include "sema_internal.h"

void sema_check_switch_stmt(Sema *s, AstNode *node) {
  Type *expr_type = sema_check_expr(s, node->switch_stmt.expr).type;
  if (!expr_type)
    return;

  SemaJump jump_ctx;
  sema_jump_push(s, &jump_ctx, sv_from_cstr(""), node, false, true);

  if ((expr_type->kind == KIND_PRIMITIVE &&
       expr_type->data.primitive == PRIM_STRING) ||
      expr_type->kind == KIND_STRING_BUFFER) {
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