#include "sema_internal.h"

AstNode *sema_coerce(Sema *s, AstNode *expr, Type *target) {
  ExprInfo expr_info = sema_check_expr(s, expr);
  Type *expr_type = expr_info.type;
  bool target_is_tagged_union =
      target && target->kind == KIND_UNION && target->is_tagged_union;

  if (expr->tag == NODE_NUMBER || expr->tag == NODE_CHAR) {
    sema_check_literal_bounds(s, expr, target);

    if (type_can_implicitly_cast(target, expr_type) ||
        (expr->tag == NODE_NUMBER && literal_fits_in_type(expr, target))) {
      if (!target_is_tagged_union) {
        expr->resolved_type = target;
      }
      return expr;
    }
  }

  if (target && target->kind == KIND_STRING_BUFFER && expr_type &&
      expr_type->kind == KIND_PRIMITIVE &&
      expr_type->data.primitive == PRIM_STRING) {
    expr = AstNode_new_cast_expr(expr, target, expr->loc);
    expr_type = target;
  }

  if (type_can_implicitly_cast(target, expr_type)) {
    if (!target_is_tagged_union) {
      expr->resolved_type = target;
    }
    return expr;
  }

  if (target->kind == KIND_PRIMITIVE &&
      target->data.primitive == PRIM_UNKNOWN) {
    expr->resolved_type = expr_type;
    return expr;
  }

  sema_error(s, expr, "Type mismatch: expected %s, got %s",
             type_to_name(target), type_to_name(expr_type));
  expr->resolved_type = expr_type;
  return expr;
}