#include "sema_internal.h"

Type *check_assignment(Sema *s, AstNode *node) {
  AstNode *target = node->assign_expr.target;
  Type *lhs = sema_check_expr(s, target);

  if (!type_is_writable(s, target)) {
    if (!type_is_lvalue(target)) {
      sema_error(s, node, "Invalid assignment target");
    } else {
      sema_error(s, node, "Cannot assign to read-only l-value");
    }
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }

  Type *rhs = sema_coerce(s, node->assign_expr.value, lhs);
  if (!type_can_implicitly_cast(lhs, rhs)) {
    sema_error(s, node, "Type mismatch in assignment: expected %s, got %s",
               type_to_name(lhs), type_to_name(rhs));
  }

  return lhs;
}

Type *check_cast(Sema *s, AstNode *node) {
  sema_check_expr(s, node->cast_expr.expr);
  return node->cast_expr.target_type;
}

Type *check_ternary(Sema *s, AstNode *node) {
  Type *cond = sema_check_expr(s, node->ternary.condition);
  if (!type_is_bool(cond)) {
    sema_error(s, node->ternary.condition,
               "Ternary condition must be boolean, got %s", type_to_name(cond));
    return type_get_primitive(s->types, PRIM_UNKNOWN);
  }
  Type *t_expr = sema_check_expr(s, node->ternary.true_expr);
  Type *f_expr = sema_check_expr(s, node->ternary.false_expr);

  if (t_expr->kind == KIND_PRIMITIVE &&
      t_expr->data.primitive == PRIM_UNKNOWN) {
    node->ternary.true_expr->resolved_type = f_expr;
    return f_expr;
  }
  if (f_expr->kind == KIND_PRIMITIVE &&
      f_expr->data.primitive == PRIM_UNKNOWN) {
    node->ternary.false_expr->resolved_type = t_expr;
    return t_expr;
  }

  if (type_can_implicitly_cast(t_expr, f_expr)) {
    check_literal_bounds(s, node->ternary.true_expr, t_expr);
    check_literal_bounds(s, node->ternary.false_expr, t_expr);
    return t_expr;
  }
  if (type_can_implicitly_cast(f_expr, t_expr)) {
    check_literal_bounds(s, node->ternary.true_expr, f_expr);
    check_literal_bounds(s, node->ternary.false_expr, f_expr);
    return f_expr;
  }
  sema_error(s, node,
             "Ternary branches must have compatible types: got %s and %s",
             type_to_name(t_expr), type_to_name(f_expr));
  return type_get_primitive(s->types, PRIM_UNKNOWN);
}
