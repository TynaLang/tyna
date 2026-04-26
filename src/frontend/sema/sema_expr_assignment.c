#include "sema_internal.h"

static bool sema_type_is_move_only(Type *type) {
  return type && type->kind == KIND_STRING_BUFFER;
}

static bool sema_result_error_set_is_wildcard(Type *error_set) {
  return error_set && error_set->kind == KIND_ERROR_SET &&
         (error_set->name.len == 0 || sv_eq_cstr(error_set->name, "Error"));
}

static void sema_result_union_error_set(Type *target_error_set,
                                        Type *source_error_set) {
  if (!target_error_set || !source_error_set ||
      target_error_set->kind != KIND_ERROR_SET ||
      source_error_set->kind != KIND_ERROR_SET) {
    return;
  }

  for (size_t i = 0; i < source_error_set->members.len; i++) {
    Member *member = source_error_set->members.items[i];
    if (!member || !member->type || member->type->kind != KIND_ERROR) {
      continue;
    }
    if (!error_set_contains(target_error_set, member->type)) {
      type_add_member(target_error_set, NULL, member->type, 0);
    }
  }
}

static Type *sema_result_type_for_assignment(Sema *s, AstNode *target,
                                             Type *lhs, Type *rhs) {
  if (!lhs || lhs->kind != KIND_RESULT) {
    return lhs;
  }

  Type *success = lhs->data.result.success;
  Type *error_set = lhs->data.result.error_set;
  bool success_changed = false;

  if (rhs && rhs->kind == KIND_RESULT) {
    if (type_is_unknown(success) && rhs->data.result.success) {
      success = rhs->data.result.success;
      success_changed = true;
    }
    if (sema_result_error_set_is_wildcard(error_set)) {
      sema_result_union_error_set(error_set, rhs->data.result.error_set);
    }
  } else if (rhs && rhs->kind == KIND_ERROR) {
    if (sema_result_error_set_is_wildcard(error_set) &&
        !error_set_contains(error_set, rhs)) {
      type_add_member(error_set, NULL, rhs, 0);
    }
  } else if (type_is_unknown(success) && rhs) {
    success = rhs;
    success_changed = true;
  }

  if (success_changed) {
    lhs = type_get_result(s->types, success, error_set);
  }

  if (target && target->tag == NODE_VAR) {
    Symbol *sym = sema_resolve(s, target->var.value);
    if (sym && sym->kind == SYM_VAR) {
      sym->type = lhs;
    }
    target->resolved_type = lhs;
  }

  return lhs;
}

ExprInfo sema_check_assignment(Sema *s, AstNode *node) {
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

  Type *rhs = sema_check_expr(s, node->assign_expr.value).type;
  lhs = sema_result_type_for_assignment(s, target, lhs, rhs);

  node->assign_expr.value = sema_coerce(s, node->assign_expr.value, lhs);
  rhs = node->assign_expr.value ? node->assign_expr.value->resolved_type
                                : type_get_primitive(s->types, PRIM_UNKNOWN);
  if (!type_can_implicitly_cast(lhs, rhs)) {
    sema_error(s, node, "Type mismatch in assignment: expected %s, got %s",
               type_to_name(lhs), type_to_name(rhs));
  }

  if (lhs && type_is_unknown(lhs) && target->tag == NODE_VAR) {
    Symbol *sym = sema_resolve(s, target->var.value);
    if (sym && sym->kind == SYM_VAR) {
      sym->type = rhs;
      target->resolved_type = rhs;
      lhs = rhs;
    }
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

ExprInfo sema_check_cast(Sema *s, AstNode *node) {
  Type *expr_type = sema_check_expr(s, node->cast_expr.expr).type;
  Type *target_type = node->cast_expr.target_type;

  if (!expr_type || !target_type) {
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  if (type_equals(expr_type, target_type)) {
    sema_warning(s, node, "Redundant cast to %s",
                 type_to_name(target_type));
    return (ExprInfo){.type = target_type, .category = VAL_RVALUE};
  }

  bool src_ptr = expr_type->kind == KIND_POINTER;
  bool dst_ptr = target_type->kind == KIND_POINTER;

  // Pointer-to-pointer cast is a bitcast/reinterpret cast.
  if (src_ptr && dst_ptr) {
    return (ExprInfo){.type = target_type, .category = VAL_RVALUE};
  }

  // Keep explicit numeric casts permissive in both widening/narrowing directions.
  if (type_is_numeric(expr_type) && type_is_numeric(target_type)) {
    return (ExprInfo){.type = target_type, .category = VAL_RVALUE};
  }

  // Otherwise require a known conversion relation.
  if (!type_can_implicitly_cast(target_type, expr_type) &&
      !type_can_implicitly_cast(expr_type, target_type)) {
    sema_error(s, node, "Invalid cast from %s to %s", type_to_name(expr_type),
               type_to_name(target_type));
    return (ExprInfo){
        .type = type_get_primitive(s->types, PRIM_UNKNOWN),
        .category = VAL_RVALUE,
    };
  }

  return (ExprInfo){
      .type = target_type,
      .category = VAL_RVALUE,
  };
}

ExprInfo sema_check_ternary(Sema *s, AstNode *node) {
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
    sema_check_literal_bounds(s, node->ternary.true_expr, t_expr);
    sema_check_literal_bounds(s, node->ternary.false_expr, t_expr);
    return (ExprInfo){.type = t_expr, .category = VAL_RVALUE};
  }
  if (type_can_implicitly_cast(f_expr, t_expr)) {
    sema_check_literal_bounds(s, node->ternary.true_expr, f_expr);
    sema_check_literal_bounds(s, node->ternary.false_expr, f_expr);
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
