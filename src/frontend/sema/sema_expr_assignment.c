#include "sema_internal.h"

static bool sema_type_is_move_only(Type *type) {
  return type && type->kind == KIND_STRING_BUFFER;
}

ExprInfo check_assignment(Sema *s, AstNode *node) {
  AstNode *target = node->assign_expr.target;
  ExprInfo lhs_info = sema_check_expr(s, target);
  Type *lhs = lhs_info.type;

  if (lhs_info.category != VAL_LVALUE || !sema_is_writable_address(s, target)) {
    if (lhs_info.category != VAL_LVALUE) {
      sema_error(s, node, "Invalid assignment target");
    } else {
      sema_error(s, node, "Cannot assign to read-only l-value");
    }
    return (ExprInfo){.type = type_get_primitive(s->types, PRIM_UNKNOWN),
                      .category = VAL_RVALUE};
  }

  node->assign_expr.value = sema_coerce(s, node->assign_expr.value, lhs);
  Type *rhs = node->assign_expr.value
                  ? node->assign_expr.value->resolved_type
                  : type_get_primitive(s->types, PRIM_UNKNOWN);
  if (!type_can_implicitly_cast(lhs, rhs)) {
    sema_error(s, node, "Type mismatch in assignment: expected %s, got %s",
               type_to_name(lhs), type_to_name(rhs));
  }

  if (sema_type_is_move_only(lhs)) {
    AstNode *rhs_var = sema_find_underlying_var(node->assign_expr.value);
    if (rhs_var && rhs_var->tag == NODE_VAR) {
      if (!(target->tag == NODE_VAR &&
            sv_eq(target->var.value, rhs_var->var.value))) {
        Symbol *sym = sema_resolve(s, rhs_var->var.value);
        if (sym && sym->kind == SYM_VAR && !sym->is_const) {
          sema_mark_moved_symbol(sym);
        }
      }
    }
  }

  sema_mark_node_requires_storage(s, target);

  return (ExprInfo){.type = lhs, .category = VAL_RVALUE};
}

ExprInfo check_cast(Sema *s, AstNode *node) {
  sema_check_expr(s, node->cast_expr.expr);
  return (ExprInfo){
      .type = node->cast_expr.target_type,
      .category = VAL_RVALUE,
  };
}

ExprInfo check_ternary(Sema *s, AstNode *node) {
  Type *cond = sema_check_expr(s, node->ternary.condition).type;
  if (!type_is_bool(cond)) {
    sema_error(s, node->ternary.condition,
               "Ternary condition must be boolean, got %s", type_to_name(cond));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }
  Type *t_expr = sema_check_expr(s, node->ternary.true_expr).type;
  Type *f_expr = sema_check_expr(s, node->ternary.false_expr).type;

  if (t_expr->kind == KIND_PRIMITIVE &&
      t_expr->data.primitive == PRIM_UNKNOWN) {
    node->ternary.true_expr->resolved_type = f_expr;
    return (ExprInfo){.type = f_expr, .category = VAL_RVALUE};
  }
  if (f_expr->kind == KIND_PRIMITIVE &&
      f_expr->data.primitive == PRIM_UNKNOWN) {
    node->ternary.false_expr->resolved_type = t_expr;
    return (ExprInfo){.type = t_expr, .category = VAL_RVALUE};
  }

  if (type_can_implicitly_cast(t_expr, f_expr)) {
    check_literal_bounds(s, node->ternary.true_expr, t_expr);
    check_literal_bounds(s, node->ternary.false_expr, t_expr);
    return (ExprInfo){.type = t_expr, .category = VAL_RVALUE};
  }
  if (type_can_implicitly_cast(f_expr, t_expr)) {
    check_literal_bounds(s, node->ternary.true_expr, f_expr);
    check_literal_bounds(s, node->ternary.false_expr, f_expr);
    return (ExprInfo){.type = f_expr, .category = VAL_RVALUE};
  }
  sema_error(s, node,
             "Ternary branches must have compatible types: got %s and %s",
             type_to_name(t_expr), type_to_name(f_expr));
  return (ExprInfo){
      .type = type_get_primitive(s->types, PRIM_UNKNOWN),
      .category = VAL_RVALUE,
  };
}
